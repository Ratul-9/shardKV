#!/bin/bash
# Creates / inspects / benchmarks / DESTROYS the EC2 cluster described in README.md. NOT RUN YET.
#   deploy/aws/cluster.sh up | status | bench | down
# Needs: aws CLI v2 configured, $AWS_REGION, $KEY_NAME, $MY_IP; optional $INSTANCE_TYPE, $REPO.
set -euo pipefail
: "${AWS_REGION:?set AWS_REGION}"
INSTANCE_TYPE=${INSTANCE_TYPE:-c7g.large}
REPO=${REPO:-$(git config --get remote.origin.url)}
COMMIT=$(git rev-parse HEAD)
TAG=project=lldstore
STATE=deploy/aws/.cluster-state   # instance ids and IPs (gitignored)
aws_() { aws --region "$AWS_REGION" "$@"; }

ami() {  # latest Ubuntu 24.04 for the instance's architecture
  local arch=arm64; [[ $INSTANCE_TYPE == c7g* || $INSTANCE_TYPE == m7g* || $INSTANCE_TYPE == t4g* ]] || arch=amd64
  aws_ ssm get-parameter --name /aws/service/canonical/ubuntu/server/24.04/stable/current/$arch/hvm/ebs-gp3/ami-id \
    --query Parameter.Value --output text
}

up() {
  : "${KEY_NAME:?set KEY_NAME}"; : "${MY_IP:?set MY_IP}"
  local sg
  sg=$(aws_ ec2 create-security-group --group-name lldstore --description "lldstore cluster" \
        --tag-specifications "ResourceType=security-group,Tags=[{Key=project,Value=lldstore}]" --query GroupId --output text)
  aws_ ec2 authorize-security-group-ingress --group-id "$sg" --protocol tcp --port 22 --cidr "$MY_IP"
  aws_ ec2 authorize-security-group-ingress --group-id "$sg" --protocol tcp --port 0-65535 --source-group "$sg"
  aws_ ssm put-parameter --name lldstore-cwagent --type String --overwrite \
      --value "$(cat deploy/aws/cloudwatch-agent.json)" >/dev/null
  local image; image=$(ami)
  # Launch the 3 nodes first with placeholder addresses to learn their private IPs, then write the
  # real user-data (instances read user-data at first boot, so we create them stopped... simpler:
  # reserve ENIs). Here: allocate 3 network interfaces first, so the IPs are known up front.
  local subnet; subnet=$(aws_ ec2 describe-subnets --filters Name=default-for-az,Values=true --query 'Subnets[0].SubnetId' --output text)
  local enis=() ips=()
  for i in 1 2 3; do
    local eni ip
    eni=$(aws_ ec2 create-network-interface --subnet-id "$subnet" --groups "$sg" \
          --tag-specifications "ResourceType=network-interface,Tags=[{Key=project,Value=lldstore}]" \
          --query NetworkInterface.NetworkInterfaceId --output text)
    ip=$(aws_ ec2 describe-network-interfaces --network-interface-ids "$eni" --query 'NetworkInterfaces[0].PrivateIpAddress' --output text)
    enis+=("$eni"); ips+=("$ip")
  done
  local raft="1=${ips[0]}:7001,2=${ips[1]}:7001,3=${ips[2]}:7001"
  local client="1=${ips[0]}:8001,2=${ips[1]}:8001,3=${ips[2]}:8001"
  : > "$STATE"
  for i in 1 2 3; do
    sed -e "s|__ROLE__|node|g" -e "s|__ID__|$i|g" -e "s|__RAFT__|$raft|g" -e "s|__CLIENT__|$client|g" \
        -e "s|__COMMIT__|$COMMIT|g" -e "s|__REPO__|$REPO|g" deploy/aws/user-data.sh > /tmp/lldstore-ud-$i.sh
    id=$(aws_ ec2 run-instances --image-id "$image" --instance-type "$INSTANCE_TYPE" --key-name "$KEY_NAME" \
          --network-interfaces "NetworkInterfaceId=${enis[$((i-1))]},DeviceIndex=0" \
          --user-data file:///tmp/lldstore-ud-$i.sh \
          --tag-specifications "ResourceType=instance,Tags=[{Key=project,Value=lldstore},{Key=Name,Value=lldstore-node$i}]" \
          --query 'Instances[0].InstanceId' --output text)
    echo "node$i $id ${ips[$((i-1))]}" >> "$STATE"
  done
  sed -e "s|__ROLE__|loadgen|g" -e "s|__ID__|0|g" -e "s|__RAFT__||g" -e "s|__CLIENT__||g" \
      -e "s|__COMMIT__|$COMMIT|g" -e "s|__REPO__|$REPO|g" deploy/aws/user-data.sh > /tmp/lldstore-ud-0.sh
  id=$(aws_ ec2 run-instances --image-id "$image" --instance-type "$INSTANCE_TYPE" --key-name "$KEY_NAME" \
        --subnet-id "$subnet" --security-group-ids "$sg" --associate-public-ip-address \
        --user-data file:///tmp/lldstore-ud-0.sh \
        --tag-specifications "ResourceType=instance,Tags=[{Key=project,Value=lldstore},{Key=Name,Value=lldstore-loadgen}]" \
        --query 'Instances[0].InstanceId' --output text)
  echo "loadgen $id" >> "$STATE"
  echo "launched (builds take ~5 minutes). State in $STATE:"; cat "$STATE"
}

loadgen_ip() {
  local id; id=$(awk '/^loadgen/ {print $2}' "$STATE")
  aws_ ec2 describe-instances --instance-ids "$id" --query 'Reservations[0].Instances[0].PublicIpAddress' --output text
}
servers() { awk '/^node/ {printf "%s%s:8001", sep, $3; sep=","}' "$STATE"; }

status() { ssh "ubuntu@$(loadgen_ip)" "kvctl --servers=$(servers) status"; }

bench() {
  local ip; ip=$(loadgen_ip)
  local out=bench/results/aws-$(date +%Y%m%d-%H%M%S); mkdir -p "$out"
  for c in 1 4 16 64; do
    ssh "ubuntu@$ip" "kvctl --servers=$(servers) bench --mode=closed --clients=$c --seconds=20 --warmup=5 --read-pct=50" | tee -a "$out/closed.txt"
  done
  for rate in 1000 5000 20000; do
    ssh "ubuntu@$ip" "kvctl --servers=$(servers) bench --mode=open --clients=16 --rate=$rate --seconds=30 --warmup=5 --read-pct=50" | tee -a "$out/open.txt"
  done
  echo "# instance=$INSTANCE_TYPE region=$AWS_REGION commit=$COMMIT" >> "$out/closed.txt"
  echo "results in $out"
}

down() {
  local ids; ids=$(aws_ ec2 describe-instances --filters "Name=tag:project,Values=lldstore" \
        "Name=instance-state-name,Values=pending,running,stopping,stopped" --query 'Reservations[].Instances[].InstanceId' --output text)
  [ -n "$ids" ] && aws_ ec2 terminate-instances --instance-ids $ids >/dev/null && aws_ ec2 wait instance-terminated --instance-ids $ids
  for eni in $(aws_ ec2 describe-network-interfaces --filters "Name=tag:project,Values=lldstore" --query 'NetworkInterfaces[].NetworkInterfaceId' --output text); do
    aws_ ec2 delete-network-interface --network-interface-id "$eni" || true
  done
  for sg in $(aws_ ec2 describe-security-groups --filters "Name=tag:project,Values=lldstore" --query 'SecurityGroups[].GroupId' --output text); do
    aws_ ec2 delete-security-group --group-id "$sg" || true
  done
  rm -f "$STATE"
  echo "deleted everything tagged $TAG. Check the console's billing page anyway."
}

case "${1:-}" in up) up ;; status) status ;; bench) bench ;; down) down ;; *) echo "usage: $0 up|status|bench|down"; exit 2 ;; esac
