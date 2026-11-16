#!/bin/bash
# CloudWatch alarms for the EC2 cluster. NOT RUN YET. Needs $AWS_REGION and $ALARM_TOPIC (SNS ARN).
set -euo pipefail
: "${AWS_REGION:?}"; : "${ALARM_TOPIC:?set ALARM_TOPIC to an SNS topic ARN}"
cw() { aws --region "$AWS_REGION" cloudwatch "$@"; }
# 1. A node stopped reporting (crash, network): no term metric for 2 minutes.
cw put-metric-alarm --alarm-name lldstore-node-silent --namespace lldstore --metric-name lldstore_raft_term \
  --statistic SampleCount --period 60 --evaluation-periods 2 --threshold 3 --comparison-operator LessThanThreshold \
  --treat-missing-data breaching --alarm-actions "$ALARM_TOPIC"
# 2. Leader churn: more than 3 elections started in 5 minutes means unstable timeouts or a flaky network.
cw put-metric-alarm --alarm-name lldstore-election-churn --namespace lldstore --metric-name lldstore_raft_elections_started \
  --statistic Maximum --period 300 --evaluation-periods 1 --threshold 3 --comparison-operator GreaterThanThreshold \
  --alarm-actions "$ALARM_TOPIC"
# 3. Slow requests: the node-local p99 summary above 50 ms. (Per node: a p99 cannot be averaged.)
cw put-metric-alarm --alarm-name lldstore-p99 --namespace lldstore --metric-name lldstore_request_latency_ns \
  --dimensions Name=quantile,Value=0.99 --statistic Maximum --period 60 --evaluation-periods 3 \
  --threshold 50000000 --comparison-operator GreaterThanThreshold --alarm-actions "$ALARM_TOPIC"
echo "alarms created"
