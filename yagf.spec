#
# spec file for package yagf (Version 0.8.2)
#
# Copyright (c) 2009 Kyrill Detinov
# This file and all modifications and additions to the pristine
# package are under the same license as the package itself.
#

# norootforbuild

Name:		yagf
Version:	0.8.7
Release:	1
URL:		http://symmetrica.net/cuneiform-linux/yagf-ru.html
License:	GPLv3+
Source0:	%{name}-%{version}.tar.gz
Group:		Productivity/Graphics/Other
Summary:	Graphical frontend for Cuneiform and Tesseract OCR tools
BuildRoot:      %{_tmppath}/%{name}-%{version}-build
BuildRequires:  libqt4-devel cmake update-desktop-files fdupes
Requires:	libqt4 >= 4.5

%description
YAGF is a graphical front-end for Cuneiform and Tesseract OCR tools.
With YAGF you can open already scanned image files or obtain new images via
XSane (scanning results are automatically passed to YAGF).
Once you have a scanned image you can prepare it for recognition, select
particular image areas for recognition, set the recognition language and so on.
Recognized text is displayed in a editor window where it can be corrected, saved
to disk or copied to clipboard.
YAGF also provides some facilities for a multi-page recognition (see the online
help for more details).

Authors:
--------
    Andrei Borovsky <anb@symmetrica.net>

%prep
%setup -q

%build
%{__mkdir} build
#cd build
cmake ./
%{__make} %{?jobs:-j %jobs}

%install
%{makeinstall}
# Create new .desktop because the original one is incorrect
%fdupes -s %{buildroot}
%suse_update_desktop_file -i YAGF

%clean
[ "%{buildroot}" != "/" ] && %{__rm} -rf %{buildroot}

%post 
#-p /sbin/ldconfig - not needed actually, since libxspreload.so doesn't need to be cached.

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root)
%doc DESCRIPTION README
%{_datadir}/%{name}
%{_libdir}/yagf
%{_bindir}/%{name}
%{_datadir}/pixmaps/%{name}.png
%dir %{_datadir}/icons/hicolor
%dir %{_datadir}/icons/hicolor/96x96
%dir %{_datadir}/icons/hicolor/96x96/apps
%{_datadir}/icons/hicolor/96x96/apps/%{name}.png
%{_datadir}/applications/YAGF.desktop

%changelog
* Sun Dec 26 2010 Andrei Borovsky - 0.8.2
- multiple selection blocks are allowed.
* Fri Jul 17 2009 Andrei Borovsky <anb@symmetrica.net> - 0.6.2
- merged the patches with the appropriate files
- removed unnessesary ldconfig call
* Wed Jul 15 2009 Kyrill Detinov <lazy.kent.suse@gmail.com> - 0.6.1
- update to 0.6.1
- fixed build in x86-64
- corrected build requires
* Sat Jun 20 2009 Kyrill Detinov <lazy.kent.suse@gmail.com> - 0.5.0
- change compiling outside of the source tree
* Mon Jun 15 2009 Kyrill Detinov <lazy.kent.suse@gmail.com> - 0.5.0
- fix requires Qt version
* Mon Jun 08 2009 Kyrill Detinov <lazy.kent.suse@gmail.com> - 0.5.0
- correct build requires:  libqt4-devel <= 4.4.3, cmake >= 2.6
* Fri Jun 05 2009 Kyrill Detinov <lazy.kent.suse@gmail.com> - 0.5.0
- initial package created
