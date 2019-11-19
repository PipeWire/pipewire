# Continuous Integration

Our CI runs on the `gitlab.freedesktop.org` infrastructure. Steps can be found
in the top-level `gitlab-ci.yml` file.

The current build happens on an x86_64, Fedora 31 image. This should be
extended to other distrubtions and architectures over time.

## fd.o Registry

The Docker image used for the build process comes from the
`registry.freedesktop.org` container registry. Images are currently manually
generate and pushed using the top-level `Dockerfile` and the following steps.
The assumption is that you are using `podman` as a `docker` alternative. The
corresponding `docker` command should be easy to find.

```sh
$ cd <pipewire-top-level-direcory>

# Build the image
$ podman build --format=docker -t registry.freedesktop.org/pipewire/pipewire/fedora:31 .

# This is usually only needed once. You will be prompted for your fd.o
# username, and the password is a personal access token that you must generate
# in the gitlab.fd.o user settings.
$ podman login

# Upload the image
$ podman push registry.freedesktop.org/pipewire/pipewire/fedora:31
```
