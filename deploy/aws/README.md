# Running lldstore on a 3-node EC2 cluster (Phase 11)

**Status: written, NOT run.** Nothing here has touched AWS yet. Read every command before running
it; they create resources that cost money until you delete them. Budget estimate is in the last
section; set a billing alarm first.

## What gets created

| Resource | Count | Why |
|---|---|---|
| Security group `lldstore` | 1 | Raft 7001, clients 8001, metrics 9101 open only inside the group; SSH only from your IP |
| EC2 instances (default `c7g.large`, Graviton, 2 vCPU, 4 GiB, EBS gp3) | 3 nodes + 1 load generator | One kvnode per instance; the load generator on its own machine, so client and server do not share cores (the Phase 2 problem, B-008) |
| CloudWatch agent on each node | 3 | Scrapes `/metrics` (Prometheus format) into CloudWatch |
| CloudWatch alarms | 3 | Node down, p99 latency, elections (leader churn) |

## Steps

```bash
export AWS_REGION=ap-south-1             # pick yours
export KEY_NAME=my-ec2-key               # an existing EC2 key pair
export MY_IP=$(curl -s https://checkip.amazonaws.com)/32
deploy/aws/cluster.sh up                 # 3 nodes + 1 load generator, prints their IPs
deploy/aws/cluster.sh status             # kvctl status from the load generator
deploy/aws/cluster.sh bench              # the same matrix as scripts/bench_cluster.sh, results copied back
deploy/aws/alarms.sh                     # CloudWatch alarms (needs an SNS topic ARN in $ALARM_TOPIC)
deploy/aws/cluster.sh down               # TERMINATE everything. Do not forget this step.
```

`cluster.sh up` uses user-data (`deploy/aws/user-data.sh`) to install a compiler, clone the repo
at the commit you are on, build the release preset, and start `kvnode` under systemd with the
other nodes' private IPs. The load generator gets the same build and runs `kvctl`.

## What to record with every number (docs/BENCHMARKS.md rule)
Instance type, region/AZ, EBS volume type and size, commit hash, the exact `kvctl` command, and the
date. Cloud numbers vary by host and neighbour; run each point 3 times and quote the median and
range, and never compare a cloud number with a bare-metal one as if they were the same setup.

## Cost (estimate, check current prices)
4 x c7g.large is roughly 4 x US$0.07/hour in most regions plus EBS: about US$0.30/hour, so a
3-hour benchmark session is about US$1. The danger is forgetting `cluster.sh down`: about
US$220/month. `cluster.sh up` tags everything `project=lldstore`, and `cluster.sh down` deletes by
that tag.
