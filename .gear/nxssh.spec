Name: nxssh
Version: 7.5
Release: alt0.1

Summary: Openssh portable (etersoft edition)

Packager: Pavel Vainerman <pv@altlinux.ru>

Group: Networking/Remote access
License: GPL, MIT/X11 for X11 bits
Url: https://github.com/openssh/openssh-portable

Source: %name-%version.tar

%description
....

%prep
%setup

%build
%autoreconf
%configure --without-zlib-version-check
%make_build || %make

%install
mkdir -p %buildroot%_bindir
install -m755 nxssh nxsshd nxssh-keygen %buildroot%_bindir/

%files
%_bindir/*


%changelog
* Wed Oct 18 2017 Pavel Vainerman <pv@altlinux.ru> 7.5-alt0.1
- initial commit

