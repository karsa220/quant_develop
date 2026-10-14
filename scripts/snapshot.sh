#!/usr/bin/env bash
# ============================================================================
# snapshot.sh —— 收盘快照落地（由 cron 在 15:15 调用）
#
# 职责：交易时段结束后，把当天的行情/信号做一次「快照」归档，供盘后研究、回测、
#       风控复核使用。本参考实现演示「定时任务触发 + 落地」这一工程形态；
#       生产环境可在此基础上：
#         - 读取共享内存 ring 做终态 dump，或
#         - 向网关发 SIGUSR1 触发其内置快照 IPC，再拷贝 /dev/shm/md_xprocess。
# ============================================================================
set -euo pipefail

ROOT=${MD_ROOT:-/opt/md-system}
LOG_DIR=${MD_LOG_DIR:-/var/log/md}
SNAP_DIR=${MD_SNAP_DIR:-/data/md/snapshots}

DATE=$(date +%Y%m%d)
TS=$(date +%Y%m%d_%H%M%S)

mkdir -p "$LOG_DIR" "$SNAP_DIR/$DATE"

echo "[snapshot] $(date -Iseconds) 开始收盘快照 ($DATE)"

# 1) 标记网关进入快照窗口（生产：向网关发信号，使其停止发布并刷尾）
pkill -USR1 -f 'gateway_main /md_xprocess' 2>/dev/null || \
    echo "[snapshot] 网关未运行，跳过信号（演示环境）"

# 2) 归档当天的网关/策略日志
if [ -f "$LOG_DIR/gateway.log" ]; then
    cp "$LOG_DIR/gateway.log" "$SNAP_DIR/$DATE/gateway_${TS}.log"
fi

# 3) 落地快照清单（生产替换为真实数据 dump）
cat > "$SNAP_DIR/$DATE/snapshot_${TS}.manifest" <<EOF
trading_day: $DATE
captured_at: $TS
ring: /dev/shm/md_xprocess
note: 参考实现仅归档日志与清单；生产应 dump ring/book 终态。
EOF

echo "[snapshot] 完成：已写入 $SNAP_DIR/$DATE/"
