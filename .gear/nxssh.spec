Name: nxssh
Version: 7.5
Release: alt0.1

Summary: Openssh portable (etersoft edition)

Packager: Pavel Vainerman <pv@altlinux.ru>

Group: Networking/Remote access
License: GPL, MIT/X11 for X11 bits
Url: https://github.com/openssh/openssh-portable

%description
....

%prep
%setup

%build
%autoreconf
%configure --without-zlib-version-check
%make_build || %make

%install
install -m755 nxssh %buildroot%_bindir/

%files
%_bindir/nxssh


%changelog
