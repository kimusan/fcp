# fcp - Faster CP
# Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
# MIT License

Name:           fcp
Version:        1.0.0
Release:        1%{?dist}
Summary:        Faster CP with progress, identical file detection, and parallelism

License:        MIT
URL:            https://github.com/kimusan/fcp

Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  openssl-devel
BuildRequires:  pkg-config

%description
fcp is a faster replacement for the standard cp command with enhanced
features including:
- Visual progress bar with ETA and speed estimation
- Identical file detection (rsync-style: size+mtime + optional SHA256)
- Parallel copy support with configurable worker count
- Reflink support for instant copies on Btrfs/XFS
- Speed limiting with --speed-limit
- Dry-run mode for previewing copies
- Colored output for better visibility

%prep
%autosetup

%build
%define _smp_mflags -j%{nil}
make CC="%{?__cc}%{!?__cc:gcc}" %{?_smp_mflags}

%install
make install DESTDIR=%{buildroot} PREFIX=/usr

%files
%doc README.md
%license LICENSE
%{_bindir}/fcp
%{_mandir}/man1/fcp.1*

%changelog
* Fri Aug 21 2026 Kim Schulz <kim@schulz.dk> - 1.0.0-1
- Initial package