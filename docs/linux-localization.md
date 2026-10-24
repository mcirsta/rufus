# Native application languages

The language button now offers the original catalog's 38 languages, in its
original order with English/native names and a checkmark on the current choice.
The bundled `res/loc/rufus.loc` remains byte-for-byte unchanged and is embedded as
a Qt resource. Arabic, Hebrew and Persian select right-to-left layout. Names
of left-to-right languages receive the original directional embedding when the
menu is shown in a right-to-left interface.

The initial language is **English**. Choose explicitly using the button or
`-l fr-FR` / `--locale=0x040c`. The exact catalog tag or numeric Windows LCID is
accepted. Numeric prefix/trailing-text handling, case-sensitive names, unknown
selection fallback and first-catalog-match resolution of duplicate LCIDs follow
`src/localization.c` and `src/rufus.c`. In particular the original Croatian and
Serbian lists share 0x081a; the earlier Croatian entry wins for that numeric ID.
A saved `Locale` takes precedence over a startup argument, as on Windows. Select
a language through the menu to replace it. Both user settings and portable
`rufus.ini` use the existing application-storage paths. No Linux locale,
keyboard, timezone or other host setting is inferred for this choice.

## Catalog reuse and native bindings

The Windows quoted-argument parser now calls the shared bounded
`src/locale_text.c` operation. Its escaped-quote behavior and original logical
line collation/escapes are reused by the native catalog reader. Windows source
lists include the shared file; Windows line endings are retained. Native parsing
supports the original language, version, base, group, text and right-to-left
attribute commands. The font command is parsed and ignored, as in Windows's
current dispatcher. Base inheritance and default-text fallback remain available;
the older Slovenian catalog can consequently fall back to English messages.

A native QTranslator binds original English texts to the same catalog IDs.
Explicit bindings cover changed capitalization, standard Qt buttons and the
main form's equivalent captions. Advanced-panel captions, boot-image prompts,
pass counts and device counts use the original templates. printf conversions
are adapted to QString arguments without executing a format string; the ordered
conversion signatures must agree with the base text. Ambiguous English labels
with differing translations, unmatched native text and incompatible placeholders
fall back to the native English source.

Switching language relabels the main form and menu, refreshes dynamic choices,
and recreates cached auxiliary dialogs on their next opening. The existing
controller, selected device/image/filesystem, labels, advanced state and complete
session log stay alive. Windows rebuilds its dialog; Qt's LanguageChange event
provides equivalent visible behavior while retaining these owned objects.
Switching is unavailable during device/image work and policy transfers. Existing
writer, helper, authorization and READY behavior is reused without changes.

**Full native translation coverage remains a parity gap.** Original matching catalog
messages and main controls are translated. Linux-specific diagnostics, file
filters and text without an original counterpart still appear in English.
This batch delivers language selection and catalog reuse; it does not claim
that every native string has a translation in all 38 languages. Remaining diagnostic bindings are tracked with that completion work.
CP68 adds the original stale-translation log notice and protects cached dialogs
from redundant initial language events.

## Adjacent catalog and validation differences

As on Windows, an adjacent `rufus.loc` overrides the embedded catalog, and an
invalid external catalog stops startup with an error. The native reader commits
a whole valid catalog rather than silently retaining partially parsed commands.
This prevents a selectable language from referring to an incomplete or cyclic
base and keeps menu/message state consistent. It accepts regular UTF-8 files
without a BOM/NUL, requires the original CRLF header within the first 1,024 bytes,
and limits a catalog to 8 MiB, 128 languages, 50,000 commands, 32-KiB logical lines
and bounded inheritance work. External English edits retain original native
source-to-ID bindings. Catalogs are user data; loading them requests no device
privilege and executes no script.

## Qualification

One grouped component/UI run on normal and ASan/UBSan/LSan builds checks every
catalog language, exact LCID resolution and duplicate precedence, metadata/base
inheritance, invalid-file refusal without replacement, quote/line boundaries,
placeholder mismatch, explicit Turkish uppercase, startup/settings precedence,
and live French/Arabic/Romanian state/log/menu/RTL behavior. Initial fixture
errors and the live heading-width repair are preserved in the checkpoint logs.

One combined normal and sanitizer networkless, diskless guest route runs the
same batch as UID1000; no USB write semantics are introduced. One full nine-suite
Qt gate and the inspected native/TGZ Preview 36 packages complete the delivery.
Existing writer/helper/boot/fault/polkit and agreed customization evidence is
reused. Compact logs, screenshots, manifests and hashes are kept under
`../.rufus-test-artifacts/cp67`; owned guest/package staging is removed after
export. Other application-update, translation, architecture/legacy, filesystem,
platform and distribution rows keep the complete Windows parity goal active.

## CP72 — catalog bindings and retained translation work

Desktop controls reuse the original USB-debug and priority templates. File filters
translate original ISO/DD/VHD/VHDX/FFU/log descriptions while preserving wildcard
suffixes and separators. All new aliases are checked against all 38 catalogs,
including the grouped `IDD_LOG/IDD_LOG` title key. Native-only descriptions may
still use English fallback. The user chose to deliver controls now and keep full
native translations separately tracked as I05 in final completion B8.

## CP78 — original catalog reuse and coverage inventory

The original 38-language catalog is reused directly. No translated catalog is
generated, and `res/loc/rufus.loc` remains unchanged. Additional native bindings
reuse the original completion-state captions, equivalent status and option
captions, format names, shortcut enabled/disabled messages and progress
templates. Qt's `%p%` progress token is passed as the original string argument.
The hash dialog's Alt+H hint and the ISOHybrid dialog now use their original
templates; the latter repeats the ISO/DD labels in the same four positions as
Windows while retaining the native dialog's two arguments.

Every reused template must retain the original ordered printf conversion
signature. A catalog message with a missing or incompatible conversion falls
back to the native English source. This includes malformed translations already
present in the original catalog; native binding does not silently repair or
execute them. Original catalog inheritance is a separate source of English
fallback, including messages absent from the older Slovenian catalog.

`linux/tests/localization_inventory.py` freezes the literal Qt sources and their
locations/hashes. It includes `tr()`, Qt static `translate()` calls,
`QT_TRANSLATE_NOOP` sources and translatable `.ui` strings. Dynamic calls and
macro-composed sources are explicitly listed for review rather than counted as
complete literals. The compiled `test_localization inventoryCoverage` operation
can consume that inventory using `RUFUS_LOCALIZATION_INVENTORY_INPUT` and write
per-language binding, placeholder-refusal and original-inheritance evidence to
`RUFUS_LOCALIZATION_INVENTORY_OUTPUT`.

On 2026-10-24 the user accepted reusing Rufus's translations with documented
English fallback for Linux-specific text. This qualifies the release's I05
catalog-reuse scope; full native translations in all 38 languages are not a
first-release completion requirement.

This inventory is source evidence rather than proof that every branch is
reachable. Technical trace logs and provider diagnostics need separate review.
Native-only UI text and diagnostics with no original equivalent remain English;
reusing the original catalogs does not establish full native translation
coverage. The additional aliases/templates are checked against all
38 catalogs, with explicit malformed-placeholder refusal cases.
