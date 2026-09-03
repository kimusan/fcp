# fcp - Faster CP

A Linux copy tool with familiar `cp` syntax, a smooth terminal progress display,
parallel directory copies, and safeguards for long-running transfers. It supports
the common `cp` workflows documented below; it is not a complete GNU `cp`
implementation.

## Features

- **Progress bar**: Smooth aggregate ETA, speed, file info, and active-worker count without hiding the completed transfer
- **Identical file detection**: Skips files only after SHA256 confirmation; timestamps avoid unnecessary hashing by default
- **Parallel copy**: Multiple worker threads for faster bulk transfers
- **Sparse file acceleration**: Linux `SEEK_DATA`/`SEEK_HOLE` extent copying without writing zero-blocks to disk
- **Archive & metadata mode**: Full preservation of permissions, ownership, timestamps, and extended attributes (`xattrs`)
- **Atomic copy mode**: Crash-resilient staging with `fdatasync()`, atomic `rename()`, and parent-directory sync
- **Reflink support**: Instant copies on Btrfs/XFS filesystems via FICLONE
- **Speed limiting**: Cap total copy bandwidth across all workers with `--speed-limit`
- **Dry-run mode**: Preview what would be copied without copying
- **Colored output**: Visual feedback with ANSI colors (auto-detected)
- **Config file**: Persistent settings in `~/.config/fcp/config`
- **Common cp-style CLI**: Familiar operands and common archive, recursive, overwrite, link, and target-directory options

## Installation

### From source

```bash
make
sudo make install
```

Or with a custom prefix:

```bash
make PREFIX=/usr
sudo make install
```

### Build Debian package

```bash
make deb
sudo dpkg -i fcp_2.1.0_amd64.deb
```

### Dependencies

- GCC or compatible C compiler
- make
- libpthread (system)
- libcrypto (OpenSSL, for SHA256)
- libm (math library)

## Usage

```bash
# Basic copy
fcp source.txt destination.txt

# Archive copy (recursive, preserve mode, ownership, timestamps, xattrs)
fcp -a src_dir/ ~/backup/

# Sparse file copy (skips holes in VM images / disk files)
fcp --sparse=auto disk.raw /mnt/backup/disk.raw

# Atomic replacement (guarantees readers never see partial copies)
fcp --atomic new_config.json /etc/app/config.json

# Parallel copy (4 workers)
fcp --parallel=4 documents/ backup/documents/

# Dry run (preview without copying)
fcp --dry-run -r src/ dest/

# Speed limit (50 MB/s)
fcp --speed-limit=50M large_file.iso ~/backup/

# Force overwrite
fcp -f important.txt ~/backup/important.txt
```

## Options

### Standard cp options

| Option | Description |
|--------|-------------|
| `-a, --archive` | Archive mode: same as `-d -r --preserve=all` |
| `-p, --preserve[=ATTRS]` | Preserve specified attributes (`mode,ownership,timestamps,xattr,all`) |
| `-r, -R, --recursive` | Copy directories recursively |
| `-i, --interactive` | Prompt before overwrite; uses one worker to keep prompts ordered |
| `-n, --no-clobber` | Do not overwrite existing files |
| `-f, --force` | Remove existing destination first |
| `-v, --verbose` | Display copied file names |
| `-d, --no-dereference` | Preserve symbolic links (the default) |
| `-L, --dereference` | Copy what symbolic links point to |
| `-s, --symbolic` | Create symlinks instead of copying |
| `-u, --update` | Copy only when source is newer |
| `-t, --target-directory=DIRECTORY` | Copy all sources into DIRECTORY |
| `-T, --no-target-directory` | Treat DESTINATION as a normal path, even if it is a directory |
| `--remove-destination` | Alias for `--force` |

### fcp-specific options

| Option | Default | Description |
|--------|---------|-------------|
| `-P, --progress` | auto | Show progress; forced even when stderr is redirected |
| `--no-progress` | - | Disable progress display |
| `--parallel=[N\|auto]` | auto | Parallel copy workers (default: nproc, max 8) |
| `--sparse=[auto\|always\|never]` | auto | Detect & accelerate sparse file copying |
| `--atomic` | off | Atomically replace destination via temp file + rename |
| `--exclude=PATTERN` | - | Exclude files matching PATTERN (glob, can be repeated) |
| `--verify-hash` | off | Use SHA256 for all same-size identical-file checks |
| `--reflink=[auto\|always\|never]` | auto | Use reflink (FICLONE) when supported |
| `--dry-run` | off | Preview without copying |
| `--speed-limit=SIZE` | no limit | Cap copy speed (e.g., `10M`, `1G`) |
| `--no-color` | auto | Disable colored output |
| `--config=PATH` | `~/.config/fcp/config` | Custom config file path |

## Configuration File

`fcp` reads settings from `~/.config/fcp/config` (or the path specified with `--config`).

Example configuration:

```ini
# fcp configuration file

# Parallel copy workers: 0=auto, 1=sequential, N=number of workers
parallel = auto

# Progress bar: auto=detect terminal, off=disable
progress = auto

# Colors: auto=detect terminal, off=disable
color = auto

# Verify identical files with SHA256 (slower but certain)
verify_hash = off

# Verbose output: auto=detect terminal, off=disable
verbose = auto

# Reflink mode: auto=use when supported, never=disable
reflink = auto

# Sparse copy mode: auto=detect sparse, always=force sparse, off=disable
sparse = auto

# Atomic copy mode: on=atomic replacement, off=direct write
atomic = off

# Speed limit (e.g., 50M, 1G)
speed_limit =
```

CLI options override config file settings. Config file settings override defaults.

## Identical File Detection

`fcp` automatically skips files that are already identical to their destination using a fast decision tree:

1. **Same inode+device**: Instant skip (hardlinked files)
2. **Different sizes**: Definitely different, copy
3. **Same size+mtime**: SHA256 comparison before skipping (default)
4. **Same size, different time, `--verify-hash`**: SHA256 comparison

This approach balances speed and accuracy, similar to rsync's strategy.

## Compatibility and Safety

`fcp` accepts the common `cp` forms (`SOURCE DESTINATION`, multiple sources to a
directory, and `-t DIRECTORY`) and options such as `-a`, `-p`, `-r`, `-i`,
`-n`, `-f`, `-d`, `-L`, `-s`, `-u`, and `-T`. Some GNU `cp` options, including
backup modes, are not implemented. Unlike GNU `cp`, `-P` means progress; use
`-d` to preserve symlinks or `-L` to dereference them.

Progress is automatic only when stderr is a terminal, so redirected output
stays clean. During a recursive scan it shows activity rather than a misleading
percentage; during copying it retains the final accounted state and summarizes
parallel activity.

For safety, fcp detects source changes during copying, reports requested
metadata/xattr preservation failures, refuses a direct symlink destination, and
rejects recursive destinations inside the source or dereferenced symlink cycles.

## Performance

`fcp` is optimized for Linux filesystems through several mechanisms:

- **copy_file_range()**: Zero-copy transfers on Linux 4.5+
- **Sparse copy**: Extent-based zero-I/O skipping with `SEEK_DATA`/`SEEK_HOLE`
- **Parallel workers**: Multiple threads for concurrent file copies
- **Reflinks**: Instant copies on Btrfs/XFS via FICLONE
- **Large buffers**: 1MB copy buffers reduce syscall overhead
- **Sequential I/O hint**: Optimizes read-ahead behavior

`copy_file_range()`, `SEEK_DATA`/`SEEK_HOLE`, and FICLONE are Linux/kernel and
filesystem dependent. In `auto` modes fcp falls back when an optimization is
unavailable; `--reflink=always` intentionally fails instead.

### Parallel Copy

Parallelism helps most when:
- Copying many small files
- Source and destination are on different disks
- Using SSDs for both source and destination

For single large files on the same disk, sequential copy is optimal.

## Packages

Pre-built binaries and native packages for **x86_64** (`amd64`) and **ARM64** (`aarch64`) are provided on the GitHub Releases page:
- **Debian / Ubuntu**: `.deb` packages
- **Fedora / RHEL / AlmaLinux**: `.rpm` packages
- **Arch Linux**: `.pkg.tar.zst` packages
- **Generic Linux**: `.tar.gz` standalone binary archives

## License

MIT License - Copyright (c) 2026 Kim Schulz <kim@schulz.dk>

See [LICENSE](LICENSE) for details.

## Development

### Building

```bash
make              # Build
make install      # Install to PREFIX (default: /usr/local)
make PREFIX=/usr  # Install to /usr
make deb          # Build Debian package
make clean        # Clean build artifacts
```

### Testing

```bash
# Test basic copy
./fcp test.txt /tmp/test_copy.txt

# Test recursive archive copy
./fcp -a src_dir/ /tmp/dst_dir/

# Test sparse copy
./fcp --sparse=auto disk.img /tmp/disk_copy.img

# Test atomic copy
./fcp --atomic test.txt /tmp/test_copy.txt

# Test parallel copy
./fcp --parallel=4 src_dir/ /tmp/dst_dir/

# Test identical detection
./fcp -v file.txt file.txt  # Should show "skipped identical"
```

### Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Run tests
5. Submit a pull request

## Roadmap

- [x] `--exclude` pattern filtering (like rsync)
- [x] Common `cp`-style operands and options
- [x] High-performance direct FICLONE reflink
- [x] Sparse file acceleration (`--sparse=auto|always|never`) with `SEEK_HOLE`/`SEEK_DATA` (v2.0)
- [x] Full metadata preservation & archive mode (`-a, --archive` / `-p, --preserve`) (v2.0)
- [x] Atomic copy mode (`--atomic`) via temporary staging + `rename()` (v2.0)
- [ ] Backup modes (`--backup`, `--suffix`)
- [ ] Compression support (zstd, lz4)
- [ ] Network copy support (via SSH)

## Reporting Bugs

Report bugs to: [kim@schulz.dk](mailto:kim@schulz.dk)

## See Also

- [cp(1)](https://man7.org/linux/man-pages/man1/cp.1.html)
- [rsync(1)](https://man7.org/linux/man-pages/man1/rsync.1.html)
