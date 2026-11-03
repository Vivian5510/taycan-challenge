#!/bin/bash
set -euxo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential gcc-13 g++-13 cmake ninja-build \
  python3 python3-venv python3-pip \
  git rsync curl wget ca-certificates xz-utils unzip file \
  libaio-dev libnuma-dev pkg-config \
  fio strace sysstat procps util-linux

update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 130
update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 130

cat >/usr/local/bin/taycan-env <<'EOF'
#!/bin/bash
set -euo pipefail
printf 'kernel=%s\n' "$(uname -srmo)"
printf 'gcc=%s\n' "$(g++ --version | head -1)"
printf 'cmake=%s\n' "$(cmake --version | head -1)"
printf 'python=%s\n' "$(python3 --version 2>&1)"
printf 'cpus=%s\n' "$(nproc)"
printf 'memory_kib=%s\n' "$(awk '/MemTotal:/ {print $2}' /proc/meminfo)"
printf 'filesystem=%s\n' "$(findmnt -no FSTYPE /var/lib/taycan-workspace 2>/dev/null || true)"
EOF
chmod +x /usr/local/bin/taycan-env
touch /etc/taycan-vm-ready
