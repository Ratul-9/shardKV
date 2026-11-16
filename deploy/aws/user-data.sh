#!/bin/bash
# EC2 user-data for one lldstore machine (templated by cluster.sh: __ROLE__, __ID__, __RAFT__,
# __CLIENT__, __COMMIT__, __REPO__). NOT RUN YET.
set -eux
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake ninja-build git
useradd --system --create-home lldstore || true
sudo -u lldstore git clone __REPO__ /home/lldstore/lldstore
cd /home/lldstore/lldstore
sudo -u lldstore git checkout __COMMIT__
sudo -u lldstore cmake --preset release -G Ninja
sudo -u lldstore cmake --build --preset release --target kvnode kvctl lincheck
install -m 755 build/release/apps/kvnode build/release/apps/kvctl build/release/apps/lincheck /usr/local/bin/
if [ "__ROLE__" = node ]; then
  mkdir -p /var/lib/lldstore && chown lldstore /var/lib/lldstore
  cat > /etc/systemd/system/kvnode.service <<UNIT
[Unit]
Description=lldstore node __ID__
After=network-online.target
[Service]
User=lldstore
ExecStart=/usr/local/bin/kvnode --id=__ID__ --raft=__RAFT__ --client=__CLIENT__ --data=/var/lib/lldstore --metrics=0.0.0.0:9101 --listen-host=0.0.0.0
Restart=on-failure
LimitNOFILE=65536
[Install]
WantedBy=multi-user.target
UNIT
  systemctl daemon-reload
  systemctl enable --now kvnode
  # CloudWatch agent: scrape /metrics (config written by cluster.sh to SSM parameter lldstore-cwagent)
  wget -q https://amazoncloudwatch-agent.s3.amazonaws.com/ubuntu/$(dpkg --print-architecture)/latest/amazon-cloudwatch-agent.deb
  dpkg -i amazon-cloudwatch-agent.deb
  /opt/aws/amazon-cloudwatch-agent/bin/amazon-cloudwatch-agent-ctl -a fetch-config -m ec2 -s -c ssm:lldstore-cwagent
fi
