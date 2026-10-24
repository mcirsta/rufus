// SPDX-License-Identifier: GPL-3.0-or-later
#include "application_language.h"
#include "application_storage.h"
#include "locale_text.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QTranslator>
#include <cerrno>
#include <cstdlib>

static void initializeLocaleResource() { Q_INIT_RESOURCE(rufus_locales); }

namespace rufus
{
namespace
{
constexpr qsizetype maximumCatalogBytes = 8 * 1024 * 1024;
constexpr size_t maximumLineBytes = 32768;
constexpr int maximumLocales = 128;
constexpr int maximumCommands = 50000;

bool quoted(QByteArray& line, size_t& position, QString& result)
{
    while (position < size_t(line.size()) && (line[position] == ' ' || line[position] == '\t')) ++position;
    size_t start = 0;
    if (!ReadLocaleQuoted(line.data(), size_t(line.size()) + 1, &position, &start)) return false;
    result = QString::fromUtf8(line.constData() + start);
    return true;
}

QString remainder(const QByteArray& line, size_t position)
{
    return QString::fromUtf8(line.constData() + position).trimmed();
}

// printf is never executed. Only value conversions that can be expressed by
// QString::arg are adapted, and their ordered types must match the base text.
QString qtFormat(QString text, QStringList& types)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    static const QRegularExpression conversion(QStringLiteral("%[-+ #0]*[0-9]*(?:\\.[0-9]+)?(?:I64|ll|[hlLzjt])?[sScCdiuoxXfFeEgGaAp]"));
    QString result;
    for (qsizetype index = 0; index < text.size();) {
        if (text[index] != QLatin1Char('%')) { result += text[index++]; continue; }
        if (index + 1 < text.size() && text[index + 1] == QLatin1Char('%')) {
            result += QLatin1Char('%'); index += 2; continue;
        }
        const auto match = conversion.match(text, index, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        if (!match.hasMatch()) { result += text[index++]; continue; }
        types.append(match.captured());
        result += QLatin1Char('%') + QString::number(types.size());
        index = match.capturedEnd();
    }
    return result;
}

bool identifier(const QString& text)
{
    static const QRegularExpression id(QStringLiteral("^[A-Za-z_][A-Za-z_0-9]*$"));
    return id.match(text).hasMatch();
}

QString originalKey(const QString& source, bool& uppercase)
{
    struct Binding { const char* source; const char* key; bool uppercase; };
    static const Binding bindings[] = {
        {"ISO images", "MSG_036", false},
        {"ISO Image", "MSG_036", false},
        {"All files", "MSG_107", false},
        {"Disk images", "MSG_095", false},
        {"Disk and ISO images", "MSG_280", false},
        {"VHDX image", "MSG_342", false},
        {"Uncompressed VHD image", "MSG_343", false},
        {"Full Flash Update image", "MSG_344", false},
        {"Log files", "IDD_LOG/IDD_LOG", false},
        {"SELECT", "IDD_DIALOG/IDC_SELECT", true},
        {"START", "IDD_DIALOG/IDC_START", true},
        {"CLOSE", "MSG_006", true},
        {"CANCEL", "MSG_007", true},
        {"CANCELLED", "MSG_211", true},
        {"FAILED", "MSG_212", true},
        {"DOWNLOAD", "MSG_040", true},
        {"Image option", "IDD_DIALOG/IDS_IMAGE_OPTION_TXT", false},
        {"Change application language", "MSG_273", false},
        {"Show log", "MSG_303", false},
        {"Show application settings", "MSG_301", false},
        {"Check now", "IDD_UPDATE_POLICY/IDC_CHECK_NOW", false},
        {"Writing master boot record...", "MSG_228", false},
        {"Writing partition boot record...", "MSG_229", false},
        {"Bad blocks detected", "MSG_010", false},
        {"VMware disk detection", "MSG_265", false},
        {"File indexing", "MSG_290", false},
        {"Persistent logging", "MSG_336", false},
        {"Listing of non-USB removable drives", "MSG_287", false},
        {"Checking for UEFI DBX updates…", "MSG_352", false},
        {"&Yes", "MSG_008", false}, {"&No", "MSG_009", false},
        {"&Cancel", "MSG_007", false}, {"&Close", "MSG_006", false},
        {"&Abort", "MSG_038", false},
        {"OK", "IDD_ABOUTBOX/IDOK", false},
    };
    for (const auto& binding : bindings) {
        if (source != QString::fromUtf8(binding.source)) continue;
        uppercase = binding.uppercase; return QString::fromLatin1(binding.key);
    }
    return {};
}

class ApplicationTranslator final : public QTranslator
{
public:
    explicit ApplicationTranslator(QObject* parent) : QTranslator(parent) {}
    bool isEmpty() const override { return false; }
    QString translate(const char*, const char* source, const char*, int n) const override
    {
        if (source == nullptr || (current == 0 && !external)) return {};
        const QString text = QString::fromUtf8(source);
        if (text == QLatin1String("%n device(s) found")) {
            const QString result = catalog.translate(current, n == 1 ? QStringLiteral("%1 device found") : QStringLiteral("%1 devices found"));
            return result.isEmpty() ? QString() : QString(result).replace(QStringLiteral("%1"), QStringLiteral("%n"));
        }
        return catalog.translate(current, text);
    }
    LocaleCatalog catalog;
    int current = 0;
    bool external = false;
};
ApplicationTranslator* translator = nullptr;

ApplicationTranslator& applicationTranslator()
{
    if (translator != nullptr) return *translator;
    initializeLocaleResource();
    translator = new ApplicationTranslator(qApp);
    QFile file(QStringLiteral(":/rufus/rufus.loc"));
    QString problem;
    if (!file.open(QIODevice::ReadOnly) || !translator->catalog.load(file.readAll(), problem))
        qFatal("Cannot load bundled Rufus languages: %s", qPrintable(problem));
    qApp->installTranslator(translator);
    return *translator;
}

void activate(ApplicationTranslator& active, int index)
{
    qApp->removeTranslator(&active);
    active.current = index;
    QApplication::setLayoutDirection(active.catalog.locales()[index].rightToLeft ? Qt::RightToLeft : Qt::LeftToRight);
    qApp->installTranslator(&active);
}
}

bool LocaleCatalog::load(const QByteArray& data, QString& problem)
{
    problem.clear();
    const bool sizeValid = data.size() >= 1024 && data.size() <= maximumCatalogBytes;
    if (!sizeValid) { problem = QStringLiteral("Invalid Rufus language catalog size."); return false; }
    // QStringDecoder returns a lazy conversion. Materialize it before checking
    // errors; Stateless also rejects an incomplete trailing UTF-8 sequence.
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString decoded = decoder(data); Q_UNUSED(decoded);
    const bool encodingValid = uchar(data[0]) < 0x80 && !data.contains('\0') && !decoder.hasError();
    if (!encodingValid || !data.first(1024).contains("\r\n")) {
        problem = QStringLiteral("Invalid Rufus language catalog size, UTF-8 encoding or CRLF header."); return false;
    }
    LocaleCatalog parsed;
    char buffer[maximumLineBytes];
    size_t position = 0;
    QString group;
    int count = 0, lineResult = 0;
    while ((lineResult = ReadLocaleLine(data.constData(), size_t(data.size()), &position, buffer, sizeof(buffer))) > 0) {
        QByteArray line(buffer);
        if (line.trimmed().isEmpty() || line.trimmed().startsWith('#')) continue;
        if (++count > maximumCommands || line.size() < 2 || (line[1] != ' ' && line[1] != '\t')) break;
        size_t cursor = 1;
        QString first, second;
        const char command = line[0];
        if (command == 'l') {
            if (parsed.locales_.size() >= maximumLocales || !quoted(line, cursor, first) || !quoted(line, cursor, second)) break;
            bool duplicate = false;
            for (const auto& locale : parsed.locales_) if (locale.tag == first) duplicate = true;
            if (duplicate || first.isEmpty() || second.isEmpty()) break;
            ApplicationLocale locale; locale.tag = first; locale.name = second;
            const QStringList ids = remainder(line, cursor).split(QLatin1Char(','));
            bool valid = !ids.isEmpty();
            for (const QString& id : ids) {
                bool numberValid = false; const unsigned number = id.trimmed().toUInt(&numberValid, 0);
                if (!numberValid || number > 0xffff) valid = false;
                locale.lcids.append(number);
            }
            if (!valid) break;
            parsed.locales_.append(locale); group.clear(); continue;
        }
        if (parsed.locales_.isEmpty()) break;
        auto& locale = parsed.locales_.last();
        if (command == 'v') {
            const QStringList version = remainder(line, cursor).split(QLatin1Char('.'));
            if (version.size() != 2 || locale.major != 0) break;
            bool majorValid = false, minorValid = false;
            locale.major = version[0].toUInt(&majorValid); locale.minor = version[1].toUInt(&minorValid);
            if (!majorValid || !minorValid || locale.major == 0) break;
        } else if (command == 'g') {
            group = remainder(line, cursor); if (!identifier(group)) break;
        } else if (command == 't') {
            while (cursor < size_t(line.size()) && (line[cursor] == ' ' || line[cursor] == '\t')) ++cursor;
            const size_t start = cursor;
            while (cursor < size_t(line.size()) && line[cursor] != ' ' && line[cursor] != '\t') ++cursor;
            first = QString::fromLatin1(line.constData() + start, qsizetype(cursor - start));
            if (!identifier(first) || !quoted(line, cursor, second) || !remainder(line, cursor).isEmpty()) break;
            const bool message = first.startsWith(QLatin1String("MSG_"));
            if (!message && group.isEmpty()) break;
            locale.entries.append({message ? first : group + QLatin1Char('/') + first, second});
        } else if (command == 'b' || command == 'a' || command == 'f') {
            if (!quoted(line, cursor, first)) break;
            if (command == 'f') {
                bool valid = false; remainder(line, cursor).toInt(&valid); if (!valid) break;
            } else {
                if (!remainder(line, cursor).isEmpty()) break;
                if (command == 'b') locale.entries.append({{}, first});
                else if (first == QLatin1String("r")) locale.rightToLeft = true;
                else break;
            }
        } else break;
    }
    const bool complete = lineResult == 0 && !parsed.locales_.isEmpty();
    if (!complete) { problem = QStringLiteral("Invalid Rufus language command near byte %1.").arg(position); return false; }
    for (int index = 0; index < parsed.locales_.size(); ++index) {
        QVector<int> stack; QHash<QString, QString> texts; int remaining = maximumCommands;
        if (parsed.locales_[index].major == 0 || !parsed.resolve(index, stack, texts, remaining)) {
            problem = QStringLiteral("Missing version, unknown base language or cyclic language inheritance."); return false;
        }
        parsed.texts_.append(texts);
    }
    for (auto entry = parsed.texts_[0].cbegin(); entry != parsed.texts_[0].cend(); ++entry) {
        QStringList types; const QString source = qtFormat(entry.value(), types);
        parsed.sourceKeys_[source].append(entry.key());
    }
    parsed.sourceTexts_ = parsed.texts_[0];
    *this = std::move(parsed); return true;
}

bool LocaleCatalog::resolve(int locale, QVector<int>& stack, QHash<QString, QString>& texts, int& remaining) const
{
    if (stack.contains(locale) || stack.size() >= maximumLocales) return false;
    stack.append(locale);
    for (const auto& entry : locales_[locale].entries) {
        if (--remaining < 0) return false;
        if (!entry.key.isEmpty()) { texts[entry.key] = entry.value; continue; }
        int base = -1;
        for (int index = 0; index < locales_.size(); ++index) if (locales_[index].tag == entry.value) base = index;
        if (base < 0 || !resolve(base, stack, texts, remaining)) return false;
    }
    stack.removeLast(); return true;
}

int LocaleCatalog::selection(const QString& choice) const
{
    if (choice.isEmpty()) return 0;
    const QByteArray bytes = choice.toLatin1();
    const bool numeric = bytes[0] >= '0' && bytes[0] <= '9';
    errno = 0;
    const unsigned long lcid = numeric ? std::strtoul(bytes.constData(), nullptr, 0) : 0;
    for (int index = 0; index < locales_.size(); ++index) {
        if (numeric && errno == 0 && lcid <= 0xffff && locales_[index].lcids.contains(unsigned(lcid))) return index;
        if (!numeric && locales_[index].tag == choice) return index;
    }
    return 0;
}

QString LocaleCatalog::text(int locale, const QString& key) const
{
    if (locale < 0 || locale >= texts_.size()) return {};
    return texts_[locale].value(key, texts_[0].value(key));
}

QString LocaleCatalog::translatedText(int locale, const QString& key) const
{
    QStringList baseTypes, translatedTypes;
    qtFormat(sourceTexts_.value(key), baseTypes);
    const QString translated = qtFormat(text(locale, key), translatedTypes);
    return baseTypes == translatedTypes ? translated : QString();
}

QString LocaleCatalog::translate(int locale, const QString& source) const
{
    if (locale < 0 || locale >= locales_.size()) return {};
    if (source.contains(QLatin1String(";;"))) {
        QStringList filters = source.split(QLatin1String(";;"));
        bool changed = false;
        for (QString& filter : filters) {
            const QString translated = translate(locale,filter);
            if (!translated.isEmpty()) { filter = translated; changed = true; }
        }
        return changed ? filters.join(QLatin1String(";;")) : QString();
    }
    const qsizetype suffix = source.lastIndexOf(QLatin1String(" ("));
    const bool fileFilter = suffix > 0 && source.endsWith(QLatin1Char(')')) && source.mid(suffix).contains(QLatin1Char('*'));
    if (fileFilter) {
        const QString translated = translate(locale,source.left(suffix));
        return translated.isEmpty() ? QString() : translated + source.mid(suffix);
    }
    const bool showDrive = source == QLatin1String("Show advanced drive properties");
    const bool hideDrive = source == QLatin1String("Hide advanced drive properties");
    const bool showFormat = source == QLatin1String("Show advanced format options");
    const bool hideFormat = source == QLatin1String("Hide advanced format options");
    if (showDrive || hideDrive || showFormat || hideFormat) {
        const QString pattern = translate(locale, showDrive || showFormat ? QStringLiteral("Show %1") : QStringLiteral("Hide %1"));
        const QString noun = text(locale, showDrive || hideDrive ? QStringLiteral("MSG_119") : QStringLiteral("MSG_120"));
        if (pattern.contains(QLatin1String("%1"))) return pattern.arg(noun);
        return {};
    }
    if (source == QLatin1String("Disk or ISO image (Please select)")) {
        const QString pattern = translate(locale, QStringLiteral("%1 (Please select)"));
        return pattern.isEmpty() ? QString() : pattern.arg(text(locale, QStringLiteral("MSG_280")));
    }
    // Native progress bars retain Qt's %p% token. The original catalog accepts
    // the already formatted percentage as a string, so pass that token through.
    struct ProgressBinding { const char* source; const char* key; };
    static const ProgressBinding progressBindings[] = {
        {"Computing image hashes: %p%", "MSG_271"},
        {"Writing image: %p%", "MSG_261"},
        {"Zeroing device: %p%", "MSG_286"},
        {"Downloading image: %p%", "MSG_241"},
    };
    for (const auto& binding : progressBindings) {
        if (source != QLatin1String(binding.source)) continue;
        const QString pattern = translatedText(locale, QString::fromLatin1(binding.key));
        return pattern.isEmpty() ? QString() : pattern.arg(QStringLiteral("%p%"));
    }
    struct ArgumentBinding { const char* source; const char* key; const char* argument; };
    static const ArgumentBinding argumentBindings[] = {
        {"Use Alt+H (in the main application window) to enable.", "MSG_311", "Alt+H"},
        {"ISOHybrid image detected", "MSG_274", "ISOHybrid"},
        {"Download grldr?", "MSG_103", "grldr"},
        {"Formatting UDF...", "MSG_222", "UDF"},
        {"Formatting exFAT...", "MSG_222", "exFAT"},
        {"Formatting NTFS...", "MSG_222", "NTFS"},
        {"Formatting ext%1...", "MSG_222", "ext%1"},
        {"Formatting FAT%1...", "MSG_222", "FAT%1"},
    };
    for (const auto& binding : argumentBindings) {
        if (source != QLatin1String(binding.source)) continue;
        const QString pattern = translatedText(locale, QString::fromLatin1(binding.key));
        return pattern.isEmpty() ? QString() : pattern.arg(QString::fromLatin1(binding.argument));
    }
    if (source == QLatin1String("Daily (Default)")) {
        const QString pattern = translatedText(locale, QStringLiteral("MSG_030"));
        return pattern.isEmpty() ? QString() : pattern.arg(text(locale, QStringLiteral("MSG_014")));
    }
    // Windows supplies the ISO and DD labels twice. Qt's native dialog reuses
    // its two arguments, retaining the same text and ordering in every locale.
    const QString hybrid = QStringLiteral(
        "The image you have selected is an 'ISOHybrid' image. This means it can be written either in "
        "%1 (file copy) mode or %2 (disk image) mode.\n"
        "Rufus recommends using %1 mode, so that you always have full access to the drive after writing it.\n"
        "However, if you encounter issues during boot, you can try writing this image again in %2 mode.\n\n"
        "Please select the mode that you want to use to write this image:");
    if (source == hybrid) {
        QString pattern = translatedText(locale, QStringLiteral("MSG_275"));
        pattern.replace(QStringLiteral("%3"), QStringLiteral("%1"));
        pattern.replace(QStringLiteral("%4"), QStringLiteral("%2"));
        return pattern;
    }
    const bool enabled = source.endsWith(QLatin1String(" enabled"));
    const bool disabled = source.endsWith(QLatin1String(" disabled"));
    if (enabled || disabled) {
        const QString noun = source.left(source.size() - (enabled ? 8 : 9));
        const QString translated = translate(locale, noun);
        const QString pattern = translatedText(locale, enabled ? QStringLiteral("MSG_250") : QStringLiteral("MSG_251"));
        if (!translated.isEmpty() && !pattern.isEmpty()) return pattern.arg(translated);
    }
    // The Windows combobox assembles these from the original count templates.
    static const QRegularExpression passes(QStringLiteral("^([124]) pass(?:es)?( \\([SMT]LC\\))?$"));
    const auto pass = passes.match(source);
    if (pass.hasMatch()) {
        if (pass.captured(1) == QLatin1String("1")) return translate(locale, QStringLiteral("%1 pass")).arg(1);
        const QString pattern = translate(locale, QStringLiteral("%1 passes %2"));
        return pattern.isEmpty() ? QString() : pattern.arg(pass.captured(1), pass.captured(2).trimmed()).trimmed();
    }
    bool uppercase = false;
    const QString binding = originalKey(source, uppercase);
    QStringList keys = binding.isEmpty() ? sourceKeys_.value(source) : QStringList{binding};
    QString result;
    for (const QString& key : keys) {
        const QString translated = translatedText(locale, key);
        if (translated.isEmpty()) continue;
        if (!result.isNull() && result != translated) return {}; // Ambiguous English labels need an explicit binding.
        result = translated;
    }
    if (uppercase) result = QLocale(locales_[locale].tag).toUpper(result);
    return result;
}

void LocaleCatalog::useSourceTexts(const LocaleCatalog& original)
{
    sourceKeys_ = original.sourceKeys_; sourceTexts_ = original.sourceTexts_;
}

const LocaleCatalog& ApplicationLanguages() { return applicationTranslator().catalog; }
QString SelectedApplicationLanguage()
{
    const auto& active = applicationTranslator(); return active.catalog.locales()[active.current].tag;
}

bool InitializeApplicationLanguage(const QString& executable, const QString& requested, QString& problem)
{
    problem.clear();
    auto& active = applicationTranslator();
    const QString external = QFileInfo(executable).dir().filePath(QStringLiteral("rufus.loc"));
    if (QFileInfo::exists(external)) {
        QFile file(external); LocaleCatalog catalog;
        const QFileInfo info(external);
        const bool regularCatalog = info.isFile() && info.size() >= 1024 && info.size() <= maximumCatalogBytes;
        if (!regularCatalog || !file.open(QIODevice::ReadOnly) || !catalog.load(file.read(maximumCatalogBytes + 1), problem)) {
            if (problem.isEmpty()) problem = QStringLiteral("Cannot read the adjacent rufus.loc language catalog.");
            return false;
        }
        catalog.useSourceTexts(active.catalog);
        active.catalog = std::move(catalog); active.external = true;
    }
    ApplicationSettings settings;
    const QString saved = settings.value(QStringLiteral("Locale")).toString();
    const int index = active.catalog.selection(saved.isEmpty() ? requested : saved);
    settings.setValue(QStringLiteral("Locale"), active.catalog.locales()[index].tag); settings.sync();
    if (settings.status() != QSettings::NoError) { problem = QStringLiteral("Cannot save the application language."); return false; }
    activate(active, index); return true;
}

bool SetApplicationLanguage(const QString& tag, QString& problem)
{
    problem.clear();
    auto& active = applicationTranslator();
    const int index = active.catalog.selection(tag);
    if (active.catalog.locales()[index].tag != tag) { problem = QStringLiteral("Unknown application language."); return false; }
    ApplicationSettings settings; settings.setValue(QStringLiteral("Locale"), tag); settings.sync();
    if (settings.status() != QSettings::NoError) { problem = QStringLiteral("Cannot save the application language."); return false; }
    if (index != active.current) activate(active, index);
    return true;
}
}
