%def_enable kerberos5

Name: nxssh
Version: 7.5
Release: alt0.3
Summary: Openssh portable (etersoft edition)

Packager: Pavel Vainerman <pv@altlinux.ru>

Group: Networking/Remote access
License: GPL, MIT/X11 for X11 bits
Url: https://github.com/openssh/openssh-portable

Source: %name-%version.tar

%if_enabled kerberos5
BuildRequires: libkrb5-devel
%endif

%description
Openssh portable (etersoft edition)

%prep
%setup

%build
with_kerberos=
%if_enabled kerberos5
with_kerberos="--with-kerberos5"
%endif


%autoreconf
%configure --without-zlib-version-check ${with_kerberos}
%make_build || %make

%install
mkdir -p %buildroot%_bindir
install -m755 nxssh nxsshd nxssh-keygen %buildroot%_bindir/

%files
%_bindir/*


%changelog
* Wed Nov 01 2017 Pavel Vainerman <pv@altlinux.ru> 7.5-alt0.3
- fixed bug in spec file (--enable-kerberos5 --> --with-kerberos5)

* Mon Oct 30 2017 Pavel Vainerman <pv@altlinux.ru> 7.5-alt0.2
- build with kerberos5 

* Wed Oct 18 2017 Pavel Vainerman <pv@altlinux.ru> 7.5-alt0.1
- initial commit

