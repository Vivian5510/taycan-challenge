#!/bin/bash
set -euxo pipefail

# CentOS 7 is EOL; use the archived vault repositories for reproducible setup.
if [ -d /etc/yum.repos.d ]; then
  mkdir -p /etc/yum.repos.d/disabled
  shopt -s nullglob
  for repo in /etc/yum.repos.d/*.repo; do
    mv "$repo" /etc/yum.repos.d/disabled/
  done
  shopt -u nullglob
  cat >/etc/yum.repos.d/CentOS-Vault.repo <<'EOF'
[base]
name=CentOS-7.9.2009 - Base
baseurl=https://vault.centos.org/7.9.2009/os/$basearch/
enabled=1
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-7

[updates]
name=CentOS-7.9.2009 - Updates
baseurl=https://vault.centos.org/7.9.2009/updates/$basearch/
enabled=1
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-7

[extras]
name=CentOS-7.9.2009 - Extras
baseurl=https://vault.centos.org/7.9.2009/extras/$basearch/
enabled=1
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-7
EOF
fi

yum clean all
yum makecache fast
yum install -y \
  ca-certificates curl wget git rsync tar xz unzip which file \
  make automake autoconf libtool pkgconfig \
  openssl-devel zlib-devel libaio-devel numactl-devel \
  strace fio sysstat procps-ng util-linux

# CMake 3.24+ is required by the participant SDK. Keep it isolated from yum.
cmake_version=3.30.9
cmake_root="/opt/cmake-${cmake_version}-linux-x86_64"
if [ ! -x "${cmake_root}/bin/cmake" ]; then
  tmp_dir=$(mktemp -d)
  curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${cmake_version}/cmake-${cmake_version}-linux-x86_64.tar.gz" -o "${tmp_dir}/cmake.tgz"
  tar -xzf "${tmp_dir}/cmake.tgz" -C /opt
  rm -rf "${tmp_dir}"
fi
ln -sfn "${cmake_root}/bin/cmake" /usr/local/bin/cmake
ln -sfn "${cmake_root}/bin/ctest" /usr/local/bin/ctest

# Prefer CentOS SCL devtoolset-8 when available, matching the target compiler.
cat >/etc/yum.repos.d/CentOS-SCLo.repo <<'EOF'
[sclo-rh]
name=CentOS-7.9.2009 - SCLo rh
baseurl=https://vault.centos.org/7.9.2009/sclo/x86_64/rh/
enabled=1
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-7

[sclo-sclo]
name=CentOS-7.9.2009 - SCLo sclo
baseurl=https://vault.centos.org/7.9.2009/sclo/x86_64/sclo/
enabled=1
gpgcheck=1
gpgkey=file:///etc/pki/rpm-gpg/RPM-GPG-KEY-CentOS-7
EOF
if yum install -y devtoolset-8-gcc devtoolset-8-gcc-c++ devtoolset-8-binutils devtoolset-8-libstdc++-devel; then
  cat >/etc/profile.d/taycan-toolchain.sh <<'EOF'
source /opt/rh/devtoolset-8/enable
EOF
  cat >/usr/local/bin/taycan-env <<'EOF'
#!/bin/bash
set -euo pipefail
source /opt/rh/devtoolset-8/enable
printf 'kernel=%s\n' "$(uname -srmo)"
printf 'gcc=%s\n' "$(g++ --version | head -1)"
printf 'cmake=%s\n' "$(cmake --version | head -1)"
printf 'python=%s\n' "$(python3 --version 2>&1)"
printf 'cpus=%s\n' "$(nproc)"
printf 'memory_kib=%s\n' "$(awk '/MemTotal:/ {print $2}' /proc/meminfo)"
EOF
else
  yum install -y gcc gcc-c++
  cat >/usr/local/bin/taycan-env <<'EOF'
#!/bin/bash
set -euo pipefail
printf 'kernel=%s\n' "$(uname -srmo)"
printf 'gcc=%s\n' "$(g++ --version | head -1)"
printf 'cmake=%s\n' "$(cmake --version | head -1)"
printf 'python=%s\n' "$(python3 --version 2>&1)"
printf 'cpus=%s\n' "$(nproc)"
printf 'memory_kib=%s\n' "$(awk '/MemTotal:/ {print $2}' /proc/meminfo)"
EOF
fi
chmod +x /usr/local/bin/taycan-env

mkdir -p /var/lib/taycan-workspace
chmod 0777 /var/lib/taycan-workspace
touch /etc/taycan-vm-ready
