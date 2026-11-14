# Build from source

On an up-to-date Omarchy or Arch system:

```sh
git clone https://github.com/28allday/omabroadcast.git ~/omabroadcast
cd ~/omabroadcast
./bin/install
```

This installs what building needs, builds the app and its own FFmpeg, makes the
package and installs it. Expect it to take a good while the first time, because
of FFmpeg. To update later, run `git pull` and `./bin/install` again. The path to
the folder cannot contain spaces.

To build without installing anything, run `./bin/build`. It leaves the program in
`build/omabroadcast`, and needs the packages listed as `makedepends` in
[pkgbuild/PKGBUILD](pkgbuild/PKGBUILD).
