/*
 *  This file is part of Poedit (https://poedit.net)
 *
 *  Copyright (C) 1999-2024 Vaclav Slavik
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE. 
 *
 */

#include "catalog.h"

#include "catalog_po.h"
#include "catalog_xliff.h"
#include "catalog_json.h"

#include "configuration.h"
#include "errors.h"
#include "extractors/extractor.h"
#include "gexecute.h"
#include "qa_checks.h"
#include "str_helpers.h"
#include "utility.h"
#include "version.h"
#include "language.h"

#include <stdio.h>
#include <wx/utils.h>
#include <wx/tokenzr.h>
#include <wx/log.h>
#include <wx/intl.h>
#include <wx/datetime.h>
#include <wx/config.h>
#include <wx/textfile.h>
#include <wx/stdpaths.h>
#include <wx/strconv.h>
#include <wx/memtext.h>
#include <wx/filename.h>

#include <algorithm>
#include <set>
#include <regex>


// ----------------------------------------------------------------------
// Textfile processing utilities:
// ----------------------------------------------------------------------

namespace
{

// Mostly correct regex for removing HTML markup
const std::wregex RE_APPROXIMATE_MARKUP(L"<[^>]*>");

// Fixup some common issues with filepaths in PO files, due to old Poedit versions,
// user misunderstanding or Poedit bugs:
wxString FixBrokenSearchPathValue(wxString p)
{
    if (p.empty())
        return p;
    // no DOS paths please:
    p.Replace("\\", "/");
    if (p.Last() == '/')
        p.RemoveLast();
    return p;
}

// Detect whether source strings are just IDs instead of actual text
bool DetectUseOfSymbolicIDs(Catalog& cat)
{
    // Employ a simple heuristic: IDs won't contain whitespace.
    // This is not enough as is, because some (notably Asian) languages don't use
    // whitespace, so also check for use of ASCII characters only. Typical non-symbolic
    // files will fail at least one of the tests in most of their strings.
    //
    for (auto& i: cat.items())
    {
        for (auto c: i->GetRawString())
        {
            if (c == ' ' || c >= 0x80)
                return false;
        }
    }

    wxLogTrace("poedit", "detected use of symbolic IDs for source language");
    return true;
}

} // anonymous namespace


// ----------------------------------------------------------------------
// Catalog::HeaderData
// ----------------------------------------------------------------------

void Catalog::HeaderData::FromString(const wxString& str)
{
    wxStringTokenizer tkn(str, "\n");
    wxString ln;

    m_entries.clear();

    while (tkn.HasMoreTokens())
    {
        ln = tkn.GetNextToken();
        size_t pos = ln.find(_T(':'));
        if (pos == wxString::npos)
        {
            wxLogError(_(L"Malformed header: “%s”"), ln.c_str());
        }
        else
        {
            Entry en;
            en.Key = wxString(ln.substr(0, pos)).Strip(wxString::both);
            en.Value = wxString(ln.substr(pos + 1)).Strip(wxString::both);

            m_entries.push_back(en);
            wxLogTrace("poedit.header",
                       "%s='%s'", en.Key.c_str(), en.Value.c_str());
        }
    }

    ParseDict();
}

wxString Catalog::HeaderData::ToString(const wxString& line_delim)
{
    UpdateDict();

    wxString hdr;
    for (auto& e: m_entries)
    {
        hdr << EscapeCString(e.Key) << ": " << EscapeCString(e.Value) << "\\n" << line_delim;
    }
    return hdr;
}

void Catalog::HeaderData::NormalizeHeaderOrder()
{
    // This is the order of header lines in a POT file
    // generated by GNU Gettext's xgettext utility, or
    // rearranged by the msgmerge utility.
    const wxString canonicalOrder[] =
    {
        "Project-Id-Version",
        "Report-Msgid-Bugs-To",
        "POT-Creation-Date",
        "PO-Revision-Date",
        "Last-Translator",
        "Language-Team",
        "Language",
        "MIME-Version",
        "Content-Type",
        "Content-Transfer-Encoding",
        "Plural-Forms"
    };

    const wxArrayString orderArr(sizeof(canonicalOrder) / sizeof(*canonicalOrder),
        canonicalOrder);

    // Sort standard header lines to the beginning of the header, in their
    // canonical order, and the rest after them, in their original order.
    std::stable_sort(m_entries.begin(), m_entries.end(),
        [&orderArr](const Entry& a, const Entry& b)
        {
            auto coalesce = [&orderArr](int x) -> int {
                if (x == wxNOT_FOUND)
                    return 1 + static_cast<int>(orderArr.GetCount());
                else
                    return x;
            };

            return coalesce(orderArr.Index(a.Key)) < coalesce(orderArr.Index(b.Key));
        });
}

void Catalog::HeaderData::UpdateDict()
{
    SetHeader("Project-Id-Version", Project);
    SetHeader("POT-Creation-Date", CreationDate);
    SetHeader("PO-Revision-Date", RevisionDate);

    if (TranslatorEmail.empty())
    {
        if (!Translator.empty() || !HasHeader("Last-Translator"))
            SetHeader("Last-Translator", Translator);
        // else: don't modify the header, leave as-is
    }
    else
    {
        if (Translator.empty())
            SetHeader("Last-Translator", TranslatorEmail);
        else
            SetHeader("Last-Translator", Translator + " <" + TranslatorEmail + ">");
    }

    SetHeader("Language-Team", LanguageTeam);
    SetHeader("MIME-Version", "1.0");
    SetHeader("Content-Type", "text/plain; charset=" + Charset);
    SetHeader("Content-Transfer-Encoding", "8bit");
    SetHeaderNotEmpty("Language", Lang.Code());
    SetHeader("X-Generator", wxString::FromAscii("Poedit " POEDIT_VERSION));

    // Set extended information:

    SetHeaderNotEmpty("X-Poedit-SourceCharset", SourceCodeCharset);

    if (!Keywords.empty())
    {
        wxString kw;
        for (size_t i = 0; i < Keywords.GetCount(); i++)
            kw += Keywords[i] + _T(';');
        kw.RemoveLast();
        SetHeader("X-Poedit-KeywordsList", kw);
    }

    SetHeaderNotEmpty("X-Poedit-Basepath", BasePath);

    unsigned i = 0;
    while (true)
    {
        wxString path;
        path.Printf("X-Poedit-SearchPath-%i", i);
        if (!HasHeader(path))
            break;
        DeleteHeader(path);
        i++;
    }

    i = 0;
    while (true)
    {
        wxString path;
        path.Printf("X-Poedit-SearchPathExcluded-%i", i);
        if (!HasHeader(path))
            break;
        DeleteHeader(path);
        i++;
    }

    for (i = 0; i < SearchPaths.size(); i++)
    {
        wxString path;
        path.Printf("X-Poedit-SearchPath-%i", i);
        SetHeader(path, SearchPaths[i]);
    }

    for (i = 0; i < SearchPathsExcluded.size(); i++)
    {
        wxString path;
        path.Printf("X-Poedit-SearchPathExcluded-%i", i);
        SetHeader(path, SearchPathsExcluded[i]);
    }

    NormalizeHeaderOrder();
}

void Catalog::HeaderData::ParseDict()
{
    wxString dummy;

    Project = GetHeader("Project-Id-Version");
    CreationDate = GetHeader("POT-Creation-Date");
    RevisionDate = GetHeader("PO-Revision-Date");

    dummy = GetHeader("Last-Translator");
    if (!dummy.empty())
    {
        wxStringTokenizer tkn(dummy, "<>");
        if (tkn.CountTokens() != 2)
        {
            Translator = dummy;
            TranslatorEmail = wxEmptyString;
        }
        else
        {
            Translator = tkn.GetNextToken().Strip(wxString::trailing);
            TranslatorEmail = tkn.GetNextToken();
        }
    }

    LanguageTeam = GetHeader("Language-Team");

    wxString ctype = GetHeader("Content-Type");
    int charsetPos = ctype.Find("; charset=");
    if (charsetPos != -1)
    {
        Charset =
            ctype.Mid(charsetPos + strlen("; charset=")).Strip(wxString::both);
    }
    else
    {
        Charset = "UTF-8";
    }

    // Parse language information, with backwards compatibility with X-Poedit-*:
    Lang = Language();
    wxString languageCode = GetHeader("Language");
    if (!languageCode.empty())
    {
        Lang = Language::TryParse(languageCode.ToStdWstring());
    }

    if (!Lang.IsValid())
    {
        // try looking for non-standard Qt extension
        languageCode = GetHeader("X-Language");
        if (!languageCode.empty())
            Lang = Language::TryParse(languageCode.ToStdWstring());
    }

    if (!Lang.IsValid())
    {
        wxString X_Language = GetHeader("X-Poedit-Language");
        wxString X_Country = GetHeader("X-Poedit-Country");
        if ( !X_Language.empty() )
            Lang = Language::FromLegacyNames(X_Language.utf8_string(), X_Country.utf8_string());
    }

    DeleteHeader("X-Poedit-Language");
    DeleteHeader("X-Poedit-Country");

    // Parse extended information:
    SourceCodeCharset = GetHeader("X-Poedit-SourceCharset");
    BasePath = FixBrokenSearchPathValue(GetHeader("X-Poedit-Basepath"));

    Keywords.Clear();
    wxString kwlist = GetHeader("X-Poedit-KeywordsList");
    if (!kwlist.empty())
    {
        wxStringTokenizer tkn(kwlist, ";");
        while (tkn.HasMoreTokens())
            Keywords.Add(tkn.GetNextToken());
    }
    else
    {
        // try backward-compatibility version X-Poedit-Keywords. The difference
        // is the separator used, see
        // http://sourceforge.net/tracker/index.php?func=detail&aid=1206579&group_id=27043&atid=389153

        wxString kw = GetHeader("X-Poedit-Keywords");
        if (!kw.empty())
        {
            wxStringTokenizer tkn(kw, ",");
            while (tkn.HasMoreTokens())
                Keywords.Add(tkn.GetNextToken());

            // and remove it, it's not for newer versions:
            DeleteHeader("X-Poedit-Keywords");
        }
    }

    SearchPaths.clear();
    int i = 0;
    while (true)
    {
        wxString path;
        path.Printf("X-Poedit-SearchPath-%i", i);
        if (!HasHeader(path))
            break;
        wxString p = FixBrokenSearchPathValue(GetHeader(path));
        if (!p.empty())
            SearchPaths.push_back(p);
        i++;
    }

    SearchPathsExcluded.clear();
    i = 0;
    while (true)
    {
        wxString path;
        path.Printf("X-Poedit-SearchPathExcluded-%i", i);
        if (!HasHeader(path))
            break;
        wxString p = FixBrokenSearchPathValue(GetHeader(path));
        if (!p.empty())
            SearchPathsExcluded.push_back(p);
        i++;
    }
}

wxString Catalog::HeaderData::GetHeader(const wxString& key) const
{
    const Entry *e = Find(key);
    if (e)
        return e->Value;
    else
        return wxEmptyString;
}

bool Catalog::HeaderData::HasHeader(const wxString& key) const
{
    return Find(key) != NULL;
}

void Catalog::HeaderData::SetHeader(const wxString& key, const wxString& value)
{
    Entry *e = (Entry*) Find(key);
    if (e)
    {
        e->Value = value;
    }
    else
    {
        Entry en;
        en.Key = key;
        en.Value = value;
        m_entries.push_back(en);
    }
}

void Catalog::HeaderData::SetHeaderNotEmpty(const wxString& key,
                                            const wxString& value)
{
    if (value.empty())
        DeleteHeader(key);
    else
        SetHeader(key, value);
}

void Catalog::HeaderData::DeleteHeader(const wxString& key)
{
    if (HasHeader(key))
    {
        Entries enew;

        for (Entries::const_iterator i = m_entries.begin();
                i != m_entries.end(); i++)
        {
            if (i->Key != key)
                enew.push_back(*i);
        }

        m_entries = enew;
    }
}

const Catalog::HeaderData::Entry *
Catalog::HeaderData::Find(const wxString& key) const
{
    size_t size = m_entries.size();
    for (size_t i = 0; i < size; i++)
    {
        if (m_entries[i].Key == key)
            return &(m_entries[i]);
    }
    return NULL;
}


// ----------------------------------------------------------------------
// Catalog class
// ----------------------------------------------------------------------

Catalog::Catalog(Type type)
{
    m_fileType = type;
    m_header.BasePath = wxEmptyString;
}


static inline wxString GetCurrentTimeString()
{
    return wxDateTime::Now().Format("%Y-%m-%d %H:%M%z");
}

void Catalog::CreateNewHeader()
{
    HeaderData &dt = Header();

    dt.CreationDate = GetCurrentTimeString();
    dt.RevisionDate = dt.CreationDate;

    dt.Lang = Language();
    if (m_fileType == Type::POT)
        dt.SetHeader("Plural-Forms", "nplurals=INTEGER; plural=EXPRESSION;"); // default invalid value

    dt.Project = wxEmptyString;
    dt.LanguageTeam = wxEmptyString;
    dt.Charset = "UTF-8";
    dt.Translator = wxConfig::Get()->Read("translator_name", wxEmptyString);
    dt.TranslatorEmail = wxConfig::Get()->Read("translator_email", wxEmptyString);
    dt.SourceCodeCharset = wxEmptyString;

    dt.BasePath = ".";

    dt.UpdateDict();
}

void Catalog::CreateNewHeader(const Catalog::HeaderData& pot_header)
{
    HeaderData &dt = Header();
    dt = pot_header;

    if ( !dt.RevisionDate.empty() )
        dt.RevisionDate = GetCurrentTimeString();

    // UTF-8 should be used by default no matter what the POT uses
    dt.Charset = "UTF-8";

    // clear the fields that are translation-specific:
    dt.Lang = Language();
    if (dt.LanguageTeam == "LANGUAGE <LL@li.org>")
        dt.LanguageTeam.clear();
    if (dt.Project == "PROJECT VERSION")
        dt.Project.clear();
    if (dt.GetHeader("Plural-Forms") == "nplurals=INTEGER; plural=EXPRESSION;")
        dt.DeleteHeader("Plural-Forms");

    // translator should be pre-filled & not the default "FULL NAME <EMAIL@ADDRESS>"
    dt.DeleteHeader("Last-Translator");
    dt.Translator = wxConfig::Get()->Read("translator_name", wxEmptyString);
    dt.TranslatorEmail = wxConfig::Get()->Read("translator_email", wxEmptyString);

    dt.UpdateDict();
}


CatalogItemPtr Catalog::FindItemByLine(int lineno)
{
    int i = FindItemIndexByLine(lineno);
    return i == -1 ? CatalogItemPtr() : m_items[i];
}

int Catalog::FindItemIndexByLine(int lineno)
{
    int last = -1;

    for (auto& i: m_items)
    {
        if (i->GetLineNumber() > lineno)
            return last;
        last++;
    }

    return last;
}


bool Catalog::RemoveSameAsSourceTranslations()
{
    bool changed = false;

    for (auto& i: m_items)
    {
        if (i ->GetString() == i->GetTranslation())
        {
            if (i->HasPlural())
            {
                // we can only easily do this operation for languages that have singular+plural, skip everything else:
                if (GetPluralFormsCount() != 2 || i->GetPluralString() != i->GetTranslation(1))
                    continue;
            }

            i->ClearTranslation();
            changed = true;
        }
    }

    return changed;
}


namespace
{

wxString MaskForType(Catalog::Type t)
{
    switch (t)
    {
        case Catalog::Type::PO:
            return MaskForType("*.po", _("PO Translation Files"));
        case Catalog::Type::POT:
            return MaskForType("*.pot", _("POT Translation Templates"));
        case Catalog::Type::XLIFF:
            return MaskForType("*.xlf;*.xliff", _("XLIFF Translation Files"));
        case Catalog::Type::JSON:
            return MaskForType("*.json", _("JSON Translation Files"));
        case Catalog::Type::JSON_FLUTTER:
            // TRANSLATORS: "Flutter" is proper noun, name of a developer tool
            return MaskForType("*.arb", _("Flutter Translation Files"));
    }
    return ""; // silence stupid warning
}

} // anonymous namespace

wxString Catalog::GetAllTypesFileMask()
{
    return MaskForType("*.po;*.pot;*.xlf;*.xliff;*.json;*.arb", _("All Translation Files"), /*showExt=*/false) +
        "|" +
        GetTypesFileMask({ Type::PO, Type::POT, Type::XLIFF, Type::JSON, Type::JSON_FLUTTER });
}

wxString Catalog::GetTypesFileMask(std::initializer_list<Type> types)
{
    if (types.size() == 0)
        return "";
    wxString out;
    auto t = types.begin();
    out += MaskForType(*t);
    for (++t; t != types.end(); ++t)
    {
        out += "|";
        out += MaskForType(*t);
    }
    return out;
}


void Catalog::SetFileName(const wxString& fn)
{
    wxFileName f(fn);
    f.MakeAbsolute();
    m_fileName = f.GetFullPath();
}


namespace
{

enum class SourcesPath
{
    Base,
    Root
};

wxString GetSourcesPath(const wxString& fileName, const Catalog::HeaderData& header, SourcesPath kind)
{
    if (fileName.empty())
        return wxString();

    if (header.BasePath.empty())
        return wxString();

    wxString basepath;
    if (wxIsAbsolutePath(header.BasePath))
    {
        basepath = header.BasePath;
    }
    else
    {
        wxString path = wxPathOnly(fileName);
        if (path.empty())
            path = ".";
        basepath = path + wxFILE_SEP_PATH + header.BasePath + wxFILE_SEP_PATH;
    }

    wxFileName root = wxFileName::DirName(basepath);
    root.MakeAbsolute();

    if (kind == SourcesPath::Root)
    {
        // Deal with misconfigured catalogs where the basepath isn't the root.
        for (auto& p : header.SearchPaths)
        {
            wxString path = (p == ".") ? basepath : basepath + wxFILE_SEP_PATH + p;
            root = CommonDirectory(root, MakeFileName(path));
        }
    }

    return root.GetFullPath();
}

} // anonymous namespace

wxString Catalog::GetSourcesBasePath() const
{
    return GetSourcesPath(m_fileName, m_header, SourcesPath::Base);
}

wxString Catalog::GetSourcesRootPath() const
{
    return GetSourcesPath(m_fileName, m_header, SourcesPath::Root);
}

bool Catalog::HasSourcesConfigured() const
{
    return !m_fileName.empty() &&
           !m_header.BasePath.empty() &&
           !m_header.SearchPaths.empty();
}

bool Catalog::HasSourcesAvailable() const
{
    if (!HasSourcesConfigured())
        return false;

    auto basepath = GetSourcesBasePath();
    if (!wxFileName::DirExists(basepath))
        return false;

    for (auto& p: m_header.SearchPaths)
    {
        auto fullp = wxIsAbsolutePath(p) ? p : basepath + p;
        if (!wxFileName::Exists(fullp))
            return false;
    }

    auto wpfile = m_header.GetHeader("X-Poedit-WPHeader");
    if (!wpfile.empty())
    {
        // The following tests in this function are heuristics, so don't run
        // them in presence of X-Poedit-WPHeader and consider the existence
        // of that file a confirmation of correct setup (even though strictly
        // speaking only its absence proves anything).
        return wxFileName::FileExists(basepath + wpfile);
    }

    if (m_header.SearchPaths.size() == 1)
    {
        // A single path doesn't give us much in terms of detection. About the
        // only thing we can do is to check if it is is a well known directory
        // that is unlikely to be the root.
        auto root = GetSourcesRootPath();
        if (root == wxGetUserHome() ||
            root == wxStandardPaths::Get().GetDocumentsDir() ||
            root.ends_with(wxString(wxFILE_SEP_PATH) + "Desktop" + wxFILE_SEP_PATH))
        {
            return false;
        }
    }

    return true;
}

std::shared_ptr<SourceCodeSpec> Catalog::GetSourceCodeSpec() const
{
    auto path = GetSourcesBasePath();
    if (!path.empty())
    {
        if (!wxFileName::DirExists(path))
            return nullptr;
    }

    auto spec = std::make_shared<SourceCodeSpec>();
    spec->BasePath = !path.empty() ? path : ".";
    spec->SearchPaths = m_header.SearchPaths;
    spec->ExcludedPaths = m_header.SearchPathsExcluded;
    spec->Charset = m_header.SourceCodeCharset;
    spec->Keywords = m_header.Keywords;
    for (auto& kv: m_header.GetAllHeaders())
        spec->XHeaders[kv.Key] = kv.Value;

    // parse file type mapping (e.g. "h=gettext:c++")
    wxStringTokenizer mapping(m_header.GetHeader("X-Poedit-Mapping"), ";");
    while (mapping.HasMoreTokens())
    {
        auto m = mapping.GetNextToken();
        spec->TypeMapping.emplace_back(m.BeforeFirst('='), m.AfterFirst('='));
    }

    return spec;
}


unsigned Catalog::GetPluralFormsCount() const
{
    unsigned count = 0;

    for (auto& i: m_items)
    {
        count = std::max(count, i->GetPluralFormsCount());
    }

    return count;
}

void Catalog::SetLanguage(Language lang)
{
    // FIXME: move m_header to POCatalog too
    m_header.Lang = lang;
}

void Catalog::GetStatistics(int *all, int *fuzzy, int *badtokens,
                            int *untranslated, int *unfinished)
{
    if (all) *all = 0;
    if (fuzzy) *fuzzy = 0;
    if (badtokens) *badtokens = 0;
    if (untranslated) *untranslated = 0;
    if (unfinished) *unfinished = 0;

    for (auto& i: m_items)
    {
        bool ok = true;

        if (all)
            (*all)++;

        if (i->IsFuzzy())
        {
            if (fuzzy)
                (*fuzzy)++;
            ok = false;
        }
        if (i->HasError())
        {
            if (badtokens)
                (*badtokens)++;
            ok = false;
        }
        if (!i->IsTranslated())
        {
            if (untranslated)
                (*untranslated)++;
            ok = false;
        }

        if ( !ok && unfinished )
            (*unfinished)++;
    }
}


void CatalogItem::SetFlags(const wxString& flags)
{
    static const wxString flag_fuzzy(wxS(", fuzzy"));

    m_moreFlags = flags;

    if (flags.find(flag_fuzzy) != wxString::npos)
    {
        m_isFuzzy = true;
        m_moreFlags.Replace(flag_fuzzy, wxString());
    }
    else
    {
        m_isFuzzy = false;
    }
}


wxString CatalogItem::GetFlags() const
{
    if (m_isFuzzy)
    {
        static const wxString flag_fuzzy(wxS(", fuzzy"));
        if (m_moreFlags.empty())
            return flag_fuzzy;
        else
            return flag_fuzzy + m_moreFlags;
    }
    else
    {
        return m_moreFlags;
    }
}

std::string CatalogItem::GetFormatFlag() const
{
    if (m_moreFlags.empty())
        return std::string();

    auto pos = m_moreFlags.find(wxS("-format"));
    if (pos == wxString::npos)
        return std::string();
    auto space = m_moreFlags.find_last_of(" \t", pos);
    auto format = (space == wxString::npos)
                    ? m_moreFlags.substr(0, pos)
                    : m_moreFlags.substr(space+1, pos-space-1);
    if (format.starts_with("no-"))
        return std::string();
    return std::string(format.begin(), format.end());
}

void CatalogItem::SetFuzzy(bool fuzzy)
{
    if (!fuzzy && m_isFuzzy)
        m_oldMsgid.clear();
    m_isFuzzy = fuzzy;

    UpdateInternalRepresentation();
}

wxString CatalogItem::GetTranslation(unsigned idx) const
{
    if (idx >= GetNumberOfTranslations())
        return wxString();
    else
        return m_translations[idx];
}

void CatalogItem::SetTranslation(const wxString &t, unsigned idx)
{
    while (idx >= m_translations.GetCount())
        m_translations.Add(wxEmptyString);
    m_translations[idx] = t;

    ClearIssue();

    m_isTranslated = true;
    for (size_t i = 0; i < m_translations.GetCount(); i++)
    {
        if (m_translations[i].empty())
        {
            m_isTranslated = false;
            break;
        }
    }

    UpdateInternalRepresentation();
}

void CatalogItem::SetTranslations(const wxArrayString &t)
{
    m_translations = t;

    ClearIssue();

    m_isTranslated = true;
    for (size_t i = 0; i < m_translations.GetCount(); i++)
    {
        if (m_translations[i].empty())
        {
            m_isTranslated = false;
            break;
        }
    }

    UpdateInternalRepresentation();
}

void CatalogItem::SetTranslationFromSource()
{
    ClearIssue();
    m_isFuzzy = false;
    m_isPreTranslated = false;
    m_isTranslated = true;

    auto iter = m_translations.begin();
    if (*iter != m_string)
    {
        *iter = m_string;
        m_isModified = true;
    }

    if (m_hasPlural)
    {
        ++iter;
        for ( ; iter != m_translations.end(); ++iter )
        {
            if (*iter != m_plural)
            {
                *iter = m_plural;
                m_isModified = true;
            }
        }
    }

    UpdateInternalRepresentation();
}

void CatalogItem::ClearTranslation()
{
    m_isFuzzy = false;
    m_isPreTranslated = false;
    m_isTranslated = false;
    for (auto& t: m_translations)
    {
        if (!t.empty())
            m_isModified = true;
        t.clear();
    }

    UpdateInternalRepresentation();
}

unsigned CatalogItem::GetPluralFormsCount() const
{
    unsigned trans = GetNumberOfTranslations();
    if ( !HasPlural() || !trans )
        return 0;

    return trans - 1;
}

wxString CatalogItem::GetOldMsgid() const
{
    wxString s;
    for (auto line: m_oldMsgid)
    {
        if (line.length() < 2)
            continue;
        if (line.Last() == '"')
            line.RemoveLast();
        if (line[0] == '"')
            line.Remove(0, 1);
        if (line.starts_with("msgid \""))
            line.Remove(0, 7);
        else if (line.starts_with("msgid_plural \""))
            line.replace(0, 14, "\n");
        s += UnescapeCString(line);
    }
    return s;
}


Catalog::ValidationResults Catalog::Validate(const wxString& /*fileWithSameContent*/)
{
    ValidationResults res;

    for (auto& i: m_items)
        i->ClearIssue();
    res.errors = 0;

    if (!HasCapability(Catalog::Cap::Translations))
        return res; // no errors in POT files

#if wxUSE_GUI
    if (Config::ShowWarnings())
    {
        // TODO: _some_ checks (e.g. plurals) do make sense even with symbolic IDs
        if (!UsesSymbolicIDsForSource())
            res.warnings = QAChecker::GetFor(*this)->Check(*this);
    }
#endif

    return res;
}


void Catalog::PostCreation()
{
    if (!m_sourceLanguage.IsValid())
    {
        if (!m_sourceIsSymbolicID)
            m_sourceIsSymbolicID = DetectUseOfSymbolicIDs(*this);

        if (!m_sourceIsSymbolicID)
        {
            // detect source language from the text (ignoring plurals for simplicity,
            // as we don't need 100% of the text):
            std::wstring allText;
            for (auto& i: items())
            {
                auto withoutMarkup = std::regex_replace(i->GetRawString().ToStdWstring(), RE_APPROXIMATE_MARKUP, L" ");
                allText.append(withoutMarkup);
                allText += L' ';
            }
            if (!allText.empty())
            {
                m_sourceLanguage = Language::TryDetectFromText(str::to_utf8(allText));
                wxLogTrace("poedit", "detected source language is '%s'", m_sourceLanguage.Code());
            }
        }
    }

    // All the following fixups are for files that contain translations (i.e. not POTs)
    if (!HasCapability(Cap::Translations))
        return;

    if (!GetLanguage().IsValid())
    {
        Language lang;
        if (!m_fileName.empty())
        {
            lang = Language::TryGuessFromFilename(m_fileName);
            wxLogTrace("poedit", "guessed translation language from filename '%s' is '%s'", m_fileName, lang.Code());
        }

        if (!lang.IsValid())
        {
            // If all else fails, try to detect the language from content
            wxString allText;
            for (auto& i: items())
            {
                if (!i->IsTranslated())
                    continue;
                allText.append(i->GetTranslation());
                allText.append('\n');
            }
            if (!allText.empty())
            {
                lang = Language::TryDetectFromText(str::to_utf8(allText));
                wxLogTrace("poedit", "detected translation language is '%s'", GetLanguage().Code());
            }
        }

        if (lang.IsValid())
            SetLanguage(lang);
    }
}



// Catalog file creation factories:

CatalogPtr Catalog::Create(Type type)
{
    switch (type)
    {
        case Type::PO:
        case Type::POT:
            return CatalogPtr(new POCatalog(type));

        case Type::XLIFF:
        case Type::JSON:
        case Type::JSON_FLUTTER:
            wxFAIL_MSG("empty XLIFF/JSON creation not implemented");
            return CatalogPtr();
    }

    return CatalogPtr(); // silence VC++ warning
}

CatalogPtr Catalog::Create(const wxString& filename, int flags)
{
    wxString ext;
    wxFileName::SplitPath(filename, nullptr, nullptr, nullptr, &ext);
    ext.MakeLower();

    CatalogPtr cat;
    if (POCatalog::CanLoadFile(ext))
    {
        cat.reset(new POCatalog(filename, flags));
        flags = 0; // don't do the stuff below that is already handled by POCatalog's parser
    }
    else if (XLIFFCatalog::CanLoadFile(ext))
    {
        cat = XLIFFCatalog::Open(filename);
    }
    else if (JSONCatalog::CanLoadFile(ext))
    {
        cat = JSONCatalog::Open(filename);
    }

    if (!cat)
        throw Exception(_("The file is in a format not recognized by Poedit."));

    if (flags & CreationFlag_IgnoreTranslations)
    {
        for (auto item: cat->m_items)
            item->ClearTranslation();
    }

    cat->SetFileName(filename);
    cat->PostCreation();

    return cat;
}

bool Catalog::CanLoadFile(const wxString& extension_)
{
    auto extension = extension_.Lower();

    return POCatalog::CanLoadFile(extension) ||
           XLIFFCatalog::CanLoadFile(extension) ||
           JSONCatalog::CanLoadFile(extension);
}


void Catalog::SideloadSourceDataFromReferenceFile(CatalogPtr ref)
{
    std::map<wxString, CatalogItemPtr> refItems;

    for (auto iref: ref->items())
        refItems[iref->GetRawString()] = iref;

    for (auto i: this->items())
    {
        auto ri = refItems.find(i->GetRawString());
        if (ri == refItems.end())
            continue;

        auto& rdata = *ri->second;
        if (rdata.GetTranslation().empty())
            continue;

        auto d = std::make_shared<SideloadedItemData>();
        d->source_string = rdata.GetTranslation();
        if (rdata.HasPlural())
            d->source_plural_string = rdata.GetTranslation(1);
        if (rdata.HasExtractedComments())
            d->extracted_comments = rdata.GetExtractedComments();

        i->AttachSideloadedData(d);
    }

    m_sideloaded = std::make_shared<SideloadedCatalogData>();
    m_sideloaded->reference_file = ref;
    m_sideloaded->source_language = ref->GetLanguage();
}

void Catalog::ClearSideloadedSourceData()
{
    m_sideloaded.reset();
    for (auto i: this->items())
        i->ClearSideloadedData();
}
