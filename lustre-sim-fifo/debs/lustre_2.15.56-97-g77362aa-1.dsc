Format: 1.0
Source: lustre
Binary: lustre-source, lustre-client-utils, lustre-server-utils, lustre-resource-agents, lustre-iokit, lustre-tests, lustre-dev, lustre-client-modules-dkms
Architecture: all i386 armhf powerpc ppc64el amd64 ia64 arm64
Version: 2.15.56-97-g77362aa-1
Maintainer: Lustre Developers <lustre-devel@lists.lustre.org>
Uploaders: Lustre Developers <lustre-devel@lists.lustre.org>
Homepage: https://wiki.whamcloud.com/
Standards-Version: 3.8.3
Vcs-Git: git://git.whamcloud.com/fs/lustre-release.git
Build-Depends: module-assistant, libreadline-dev, debhelper (>= 11), dpatch, automake (>= 1.7) | automake1.7 | automake1.8 | automake1.9, pkg-config, libtool, libyaml-dev, libnl-genl-3-dev, libselinux-dev, libsnmp-dev, mpi-default-dev, bzip2, quilt, linux-headers-generic | linux-headers-amd64 | linux-headers-arm64, rsync, libssl-dev, libpython3-dev, swig, libmount-dev, ed
Package-List:
 lustre-client-modules-dkms deb admin optional arch=i386,armhf,powerpc,ppc64el,amd64,ia64
 lustre-client-utils deb utils optional arch=i386,armhf,powerpc,ppc64el,amd64,ia64,arm64
 lustre-dev deb libdevel optional arch=i386,armhf,powerpc,ppc64el,amd64,ia64,arm64
 lustre-iokit deb utils optional arch=i386,armhf,powerpc,ppc64el,amd64,ia64,arm64
 lustre-resource-agents deb ha optional arch=i386,armhf,powerpc,ppc64el,amd64,ia64,arm64
 lustre-server-utils deb utils optional arch=i386,armhf,powerpc,ppc64el,amd64,ia64,arm64
 lustre-source deb admin optional arch=all
 lustre-tests deb utils optional arch=i386,armhf,powerpc,ppc64el,amd64,ia64,arm64
Checksums-Sha1:
 1abe0f3731d8d95ede3f435a82d22747859072b3 24852885 lustre_2.15.56-97-g77362aa-1.tar.gz
Checksums-Sha256:
 23165fc929697aad4ef70ba55741a3ee9f789d8d3e2024085079fac4e7f2abf4 24852885 lustre_2.15.56-97-g77362aa-1.tar.gz
Files:
 fe4c1b1fb0ff415e3c21ac2ac21878f7 24852885 lustre_2.15.56-97-g77362aa-1.tar.gz
