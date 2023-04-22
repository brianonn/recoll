#
# Derived from the Fedora .spec, with KIO and chmlib disabled to avoid non
# standard deps  + small adjustements like specifying python2 explicitely
#

# Turn off the brp-python-bytecompile script
%global __os_install_post %(echo '%{__os_install_post}' | sed -e 's!/usr/lib[^[:space:]]*/brp-python-bytecompile[[:space:]].*$!!g')

Summary:        Desktop full text search tool with Qt GUI
Name:           recoll
Version:        1.33.5
Release:        2%{?dist}
Group:          Applications/Databases
License:        GPLv2+
URL:            http://www.lesbonscomptes.com/recoll/
Source0:        http://www.lesbonscomptes.com/recoll/recoll-%{version}.tar.gz
Source10:       qmake-qt5.sh
BuildRequires:  aspell-devel
BuildRequires:  bison
BuildRequires:  desktop-file-utils
# kio
#BuildRequires:  kdelibs4-devel
BuildRequires:  qt5-qtbase-devel
#BuildRequires:  qt5-qtwebkit-devel
#BuildRequires:  extra-cmake-modules
#BuildRequires:  kf5-kio-devel
#BuildRequires:  python2-devel
#BuildRequires:  python3-devel
BuildRequires:  xapian-core-devel
BuildRequires:  zlib-devel
BuildRequires:  libxslt-devel
Requires:       xdg-utils

%description
Recoll is a personal full text search package for Linux, FreeBSD and
other Unix systems. It is based on the powerful Xapian backend, for
which it provides an easy to use, feature-rich, easy administration
interface.


%prep
%setup -q -n %{name}-%{version}

%build
CFLAGS="%{optflags}"; export CFLAGS
CXXFLAGS="%{optflags}"; export CXXFLAGS
LDFLAGS="%{?__global_ldflags}"; export LDFLAGS

# force use of custom/local qmake, to inject proper build flags (above)
install -m755 -D %{SOURCE10} qmake-qt5.sh
export QMAKE=qmake-qt5

%configure --disable-python-chm --disable-python-module --disable-webkit --disable-webengine --disable-python-module
make %{?_smp_mflags}

%install
make install DESTDIR=%{buildroot} STRIP=/bin/true INSTALL='install -p'

desktop-file-install --delete-original \
  --dir=%{buildroot}/%{_datadir}/applications \
  %{buildroot}/%{_datadir}/applications/%{name}-searchgui.desktop

# use /usr/bin/xdg-open
rm -f %{buildroot}/usr/share/recoll/filters/xdg-open

mkdir -p %{buildroot}%{_sysconfdir}/ld.so.conf.d
echo "%{_libdir}/recoll" > %{buildroot}%{_sysconfdir}/ld.so.conf.d/%{name}-%{_arch}.conf

%post
touch --no-create %{_datadir}/icons/hicolor
if [ -x %{_bindir}/gtk-update-icon-cache ] ; then
  %{_bindir}/gtk-update-icon-cache --quiet %{_datadir}/icons/hicolor
fi
if [ -x %{_bindir}/update-desktop-database ] ; then
  %{_bindir}/update-desktop-database &> /dev/null
fi
/sbin/ldconfig
exit 0

%postun
touch --no-create %{_datadir}/icons/hicolor 
if [ -x %{_bindir}/gtk-update-icon-cache ] ; then
  %{_bindir}/gtk-update-icon-cache --quiet %{_datadir}/icons/hicolor
fi
if [ -x %{_bindir}/update-desktop-database ] ; then
  %{_bindir}/update-desktop-database &> /dev/null
fi
/sbin/ldconfig
exit 0

%files
%license COPYING
%doc ChangeLog README
%{_sysconfdir}/ld.so.conf.d/%{name}-%{_arch}.conf
%{_bindir}/%{name}
%{_bindir}/%{name}index
%{_datadir}/%{name}
%{_datadir}/metainfo/%{name}.appdata.xml
%{_datadir}/applications/%{name}-searchgui.desktop
%{_datadir}/icons/hicolor/48x48/apps/%{name}.png
%{_datadir}/pixmaps/%{name}.png
%{_libdir}/recoll
%{_mandir}/man1/%{name}.1*
%{_mandir}/man1/%{name}q.1*
%{_mandir}/man1/%{name}index.1*
%{_mandir}/man5/%{name}.conf.5*
%{_unitdir}/recollindex@.service
%{_userunitdir}/recollindex.service


%changelog
* Sat Apr 22 2023 J.F. Dockes <jf@dockes.org> - 1.33.5-2
- Special centos7 build

* Wed Feb  1 2006 Jean-Francois Dockes <jfd@recoll.org> 1.2.0-1
- initial packaging
