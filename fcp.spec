# fcp - Faster CP
# Copyright (c) 2026 Kim Schulz <kim@schulz.dk>
# MIT License

Name:           fcp
Version:        2.1.0
Release:        1%{?dist}
Summary:        Linux copy tool with progress, safety checks, and parallelism

License:        MIT
URL:            https://github.com/kimusan/fcp

Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  openssl-devel
BuildRequires:  pkg-config

%description
fcp is a Linux copy tool with familiar cp invocation forms and enhanced
features including:
- Smooth terminal progress with ETA, rate, and parallel activity
- Identical file detection (rsync-style: size+mtime + optional SHA256)
- Parallel copy support with configurable worker count
- Reflink support for instant copies on Btrfs/XFS
- Sparse file acceleration with SEEK_DATA/SEEK_HOLE
- Full metadata preservation and archive mode (-a, -p)
- Atomic file replacement mode (--atomic)
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
* Thu Sep 03 2026 Kim Schulz <kim@schulz.dk> - 2.1.0-1
- Improve common cp option compatibility, including -d, -L, -T, and
  --remove-destination.
- Improve progress rendering, parallel recursive copy behavior, and transfer
  safety checks.

* Thu Sep 03 2026 Kim Schulz <kim@schulz.dk> - 2.0.1-1
- Synchronize package metadata with the 2.0.1 release.

* Fri Aug 21 2026 Kim Schulz <kim@schulz.dk> - 2.0.0-1
- fcp v2.0.0 release with sparse copy, archive/metadata mode, and atomic copy

* Fri Aug 21 2026 Kim Schulz <kim@schulz.dk> - 1.0.0-1
- Initial package
