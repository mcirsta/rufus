SUBDIRS = ../.mingw bled ext2fs ms-sys syslinux/libfat syslinux/libinstaller syslinux/win libcdio/iso9660 libcdio/udf libcdio/driver wimlib ../res/loc
# As far as I can tell, the following libraries are *not* vulnerable to side-loading, so we link using their regular version:
NONVULNERABLE_LIBS = -lole32 -lgdi32 -lshlwapi -lcomctl32 -luuid -lntdll
# The following libraries are vulnerable (or have an unknown vulnerability status), so we link using our delay-loaded replacement:
# See https://github.com/pbatard/rufus/issues/2272
VULNERABLE_LIBS = -lcrypt32-delaylib -ldwmapi-delaylib -lsetupapi-delaylib -luxtheme-delaylib -lversion-delaylib -lvirtdisk-delaylib -lwininet-delaylib -lwintrust-delaylib

noinst_PROGRAMS = rufus

AM_V_WINDRES_0 = @echo "  RC     $@";$(WINDRES)
AM_V_WINDRES_1 = $(WINDRES)
AM_V_WINDRES_  = $(AM_V_WINDRES_$(AM_DEFAULT_VERBOSITY))
AM_V_WINDRES   = $(AM_V_WINDRES_$(V))

%_rc.o: %.rc ../res/loc/embedded.loc
	$(AM_V_WINDRES) $(AM_RCFLAGS) -i $< -o $@

rufus_SOURCES = badblocks.c darkmode.c dev.c dos.c dos_locale.c dos_locale_policy.c dos_locale_policy.h freedos_files.c freedos_files.h freedos_files.inc msdos_files.c msdos_files.h drive_label.c drive_label.h locale_text.c locale_text.h drive.c format.c format_ext.c format_fat32.c hash.c hash_core.h hash_core.inc icon.c config_text.c config_text.h format_policy.c format_policy.h progress_policy.c progress_policy.h hdd_score.c hdd_score.h hdd_score_strings.h unattend.c unattend.h image_file.c image_file.h image_compression.h image_policy.c image_policy.h partition_layout.c partition_layout.h image_report.h iso.c iso_analyze.c iso_analyze.h iso_bootloaders.c iso_bootloaders.h embedded_efi.c embedded_efi.h iso_file.c iso_file.h winpe_scan.c winpe_scan.h winpe_setup.c winpe_setup.h wim_metadata.c wim_metadata.h pe_arch.c pe_arch.h iso_read.c iso_read.h iso_scan.c iso_scan.h localization.c \
	 net.c parser.c pki.c secure_boot_db.h download_key.h download_urls.h process.c cregex_compile.c cregex_parse.c cregex_vm.c rufus.c smart.c stdfn.c strarray.c strarray.h stdio.c stdlg.c syslinux.c syslinux_version.c syslinux_version.h grub_inspect.c grub_inspect.h efi_boot_info.c efi_boot_info.h syslinux_scan.c syslinux_scan.h ui.c vhd.c wue.c xml.c
rufus_CFLAGS = -I$(srcdir)/ms-sys/inc -I$(srcdir)/syslinux/libfat -I$(srcdir)/syslinux/libinstaller -I$(srcdir)/syslinux/win -I$(srcdir)/libcdio -I$(srcdir)/wimlib -I$(srcdir)/../res $(AM_CFLAGS) \
	-DEXT2_FLAT_INCLUDES=0 -D_RUFUS -DSOLUTION=rufus
rufus_LDFLAGS = $(AM_LDFLAGS) -mwindows -L../.mingw
rufus_LDADD = rufus_rc.o bled/libbled.a ext2fs/libext2fs.a ms-sys/libmssys.a syslinux/libfat/libfat.a syslinux/libinstaller/libinstaller.a syslinux/win/libwin.a \
	libcdio/iso9660/libiso9660.a libcdio/udf/libudf.a libcdio/driver/libdriver.a wimlib/libwim.a $(NONVULNERABLE_LIBS) $(VULNERABLE_LIBS)
