# DoWrite

DoWrite is a small Linux CLI tool for writing disk images, such as ISO
or raw image files, directly to block devices.

## Build

```sh
make
```

This builds the `dowrite` binary in the project directory.

To install:

```sh
sudo make install
```

By default this installs to `/usr/local/bin`. You can override `PREFIX`:

```sh
sudo make PREFIX=/usr install
```

To remove an installed binary:

```sh
sudo make uninstall
```

## Usage

```sh
dowrite [--yes] <image.iso> <device>
```

Example:

```sh
sudo dowrite alpinelinux.iso /dev/sda
```

DoWrite prints the source and target, then asks you to type the target
path to confirm. During the write it shows progress, average speed, and ETA.

Use `--yes` to skip the interactive confirmation:

```sh
sudo dowrite --yes image.iso /dev/sda
```

Other options:

```sh
dowrite --help
dowrite --version
```

## Notes

Double-check the target device before writing. Choosing the wrong device can destroy data.

This tool currently targets Linux images. Windows images may boot, but installation is not expected to work.
