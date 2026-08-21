# fcp - Faster CP

A faster replacement for the classic `cp` command with modern features including visual progress bars, identical file detection, parallel copy support, and more.

## Features

- **Progress bar**: Visual progress with ETA, speed, and file info
- **Identical file detection**: Skips files that are already copied (rsync-style: size+mtime + optional SHA256)
- **Parallel copy**: Multiple worker threads for faster bulk transfers
- **Reflink support**: Instant copies on Btrfs/XFS filesystems
- **Speed limiting**: Cap copy bandwidth with `--speed-limit`
- **Dry-run mode**: Preview what would be copied without copying
- **Colored output**: Visual feedback with ANSI colors (auto-detected)
- **Config file**: Persistent settings in `~/.config/fcp/config`
- **cp compatible**: Full command-line compatibility with standard `cp`

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
sudo dpkg -i fcp_1.0.0_amd64.deb
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

# Recursive copy with progress
fcp -r photos/ ~/backup/photos/

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
| `-r, -R, --recursive` | Copy directories recursively |
| `-i, --interactive` | Prompt before overwrite |
| `-n, --no-clobber` | Do not overwrite existing files |
| `-f, --force` | Remove existing destination first |
| `-v, --verbose` | Display copied file names |
| `-d, --dereference` | Copy what symlinks point to |
| `-s, --symbolic` | Create symlinks instead of copying |
| `-u, --update` | Copy only when source is newer |
| `-t, --target-directory=DIRECTORY` | Copy all sources into DIRECTORY |

### fcp-specific options

| Option | Default | Description |
|--------|---------|-------------|
| `-P, --progress` | auto | Show progress bar |
| `--no-progress` | - | Disable progress display |
| `--parallel=[N\|auto]` | auto | Parallel copy workers (default: nproc, max 8) |
| `--exclude=PATTERN` | - | Exclude files matching PATTERN (glob, can be repeated) |
| `--verify-hash` | off | Use SHA256 for identical detection |
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

# Speed limit (e.g., 50M, 1G)
speed_limit =
```

CLI options override config file settings. Config file settings override defaults.

## Identical File Detection

`fcp` automatically skips files that are already identical to their destination using a fast decision tree:

1. **Same inode+device**: Instant skip (hardlinked files)
2. **Different sizes**: Definitely different, copy
3. **Same size+mtime**: Likely identical, skip (default)
4. **Same size, different time, `--verify-hash`**: SHA256 comparison

This approach balances speed and accuracy, similar to rsync's strategy.

## Performance

`fcp` is optimized for speed through several mechanisms:

- **copy_file_range()**: Zero-copy transfers on Linux 4.5+
- **Parallel workers**: Multiple threads for concurrent file copies
- **Reflinks**: Instant copies on Btrfs/XFS via FICLONE
- **Large buffers**: 1MB copy buffers reduce syscall overhead
- **Sequential I/O hint**: Optimizes read-ahead behavior

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

# Test recursive copy
./fcp -r src_dir/ /tmp/dst_dir/

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
- [x] Full CLI options & standard `cp` compatibility
- [x] High-performance direct FICLONE reflink
- [ ] Sparse file acceleration (`--sparse=auto|always|never`) with `SEEK_HOLE`/`SEEK_DATA` (v2.0)
- [ ] Full metadata preservation & archive mode (`-a, --archive` / `-p, --preserve`) (v2.0)
- [ ] Atomic copy mode (`--atomic`) via temporary swap (v2.0)
- [ ] Backup modes (`--backup`, `--suffix`)
- [ ] Compression support (zstd, lz4)
- [ ] Network copy support (via SSH)

## Reporting Bugs

Report bugs to: [kim@schulz.dk](mailto:kim@schulz.dk)

## See Also

- [cp(1)](https://man7.org/linux/man-pages/man1/cp.1.html)
- [rsync(1)](https://man7.org/linux/man-pages/man1/rsync.1.html)