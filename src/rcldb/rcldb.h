/* Copyright (C) 2004-2021 J.F.Dockes
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#ifndef _DB_H_INCLUDED_
#define _DB_H_INCLUDED_

#include "autoconfig.h"

#include <stdint.h>
#include <string>
#include <vector>
#include <memory>

#include "cstr.h"
#include "rcldoc.h"
#include "stoplist.h"
#include "rclconfig.h"
#include "utf8iter.h"
#include "textsplit.h"
#include "syngroups.h"

using std::string;
using std::vector;

// rcldb defines an interface for a 'real' text database. The current 
// implementation uses xapian only, and xapian-related code is in rcldb.cpp
// If support was added for other backend, the xapian code would be moved in 
// rclxapian.cpp, another file would be created for the new backend, and the
// configuration/compile/link code would be adjusted to allow choosing. There 
// is no plan for supporting multiple different backends.
// 
// In no case does this try to implement a useful virtualized text-db interface
// The main goal is simplicity and good matching to usage inside the recoll
// user interface. In other words, this is not exhaustive or well-designed or 
// reusable.
//
// Unique Document Identifier: uniquely identifies a document in its
// source storage (file system or other). Used for up to date checks
// etc. "udi". Our user is responsible for making sure it's not too
// big, cause it's stored as a Xapian term (< 150 bytes would be
// reasonable)

class RclConfig;
class Aspell;

namespace Rcl {

class DbW;

// Omega compatible values. We leave a hole for future omega values. Not sure 
// it makes any sense to keep any level of omega compat given that the index
// is incompatible anyway.
enum value_slot {
    // Omega-compatible values:
    VALUE_LASTMOD = 0,  // 4 byte big endian value - seconds since 1970.
    VALUE_MD5 = 1,      // 16 byte MD5 checksum of original document.
    VALUE_SIZE = 2,     // sortable_serialise(<file size in bytes>)

    ////////// Recoll only:
    // Doc sig as chosen by app (ex: mtime+size
    VALUE_SIG = 10,
};

class SearchData;
class TermIter;
class Query;

/** Used for returning result lists for index terms matching some criteria */
class TermMatchEntry {
public:
    TermMatchEntry() 
        : wcf(0) {}
    TermMatchEntry(const string& t, int f, int d)
        : term(t), wcf(f), docs(d) {}
    TermMatchEntry(const string& t)
        : term(t), wcf(0) {}
    bool operator==(const TermMatchEntry &o) const { 
        return term == o.term;
    }
    bool operator<(const TermMatchEntry &o) const { 
        return term < o.term;
    }

    string term;
    int    wcf; // Total count of occurrences within collection.
    int    docs; // Number of documents countaining term.
};

/** Term match result list header: statistics and global info */
class TermMatchResult {
public:
    TermMatchResult() {
        clear();
    }
    void clear() {
        entries.clear(); 
    }
    // Term expansion
    vector<TermMatchEntry> entries;
    // If a field was specified, this is the corresponding index prefix
    string prefix;
};

class DbStats {
public:
    DbStats() {}
    // Index-wide stats
    unsigned int dbdoccount{0};
    double       dbavgdoclen{0};
    size_t       mindoclen{0};
    size_t       maxdoclen{0};
    std::vector<std::string> failedurls; /* Only set if requested */
};

inline bool has_prefix(const string& trm)
{
    if (o_index_stripchars) {
        return !trm.empty() && 'A' <= trm[0] && trm[0] <= 'Z';
    } else {
        return !trm.empty() && trm[0] == ':';
    }
}

inline string strip_prefix(const string& trm)
{
    if (!has_prefix(trm))
        return trm;
    string::size_type st = 0;
    if (o_index_stripchars) {
        st = trm.find_first_not_of("ABCDEFIJKLMNOPQRSTUVWXYZ");
#ifdef _WIN32
        // We have a problem there because we forgot to lowercase the drive
        // name. So if the found character is a colon consider the drive name as
        // the first non capital even if it is uppercase
        if (st != string::npos && st >= 2 && trm[st] == ':') {
            st -= 1;
        }
#endif
    } else {
        st = trm.find_first_of(":", 1) + 1;
    }
    if (st == string::npos) {
        return string(); // ??
    }
    return trm.substr(st);
}

inline string get_prefix(const string& trm)
{
    if (!has_prefix(trm))
        return string();
    string::size_type st = 0;
    if (o_index_stripchars) {
        st = trm.find_first_not_of("ABCDEFIJKLMNOPQRSTUVWXYZ");
        if (st == string::npos) {
            return string(); // ??
        }
#ifdef _WIN32
        // We have a problem there because we forgot to lowercase the drive
        // name. So if the found character is a colon consider the drive name as
        // the first non capital even if it is uppercase
        if (st >= 2 && trm[st] == ':') {
            st -= 1;
        }
#endif
        return trm.substr(0, st);
    } else {
        st = trm.find_first_of(":", 1) + 1;
        if (st == string::npos) {
            return string(); // ??
        }
        return trm.substr(1, st-2);
    }
}

inline string wrap_prefix(const string& pfx) 
{
    if (o_index_stripchars) {
        return pfx;
    } else {
        return cstr_colon + pfx + cstr_colon;
    }
}

/**
 * Wrapper class for the native database.
 */
class Db {
public:
    /* General stuff (valid for query or update) ****************************/
    Db(const RclConfig *cfp);
    ~Db();
    Db(const Db &) = delete;
    Db& operator=(const Db &) = delete;

    enum OpenMode {DbRO, DbUpd, DbTrunc};
    bool isWriteMode(OpenMode mode) {
        return mode == DbUpd || mode == DbTrunc;
    }
    enum OpenError {DbOpenNoError, DbOpenMainDb, DbOpenExtraDb};
    bool open(OpenMode mode, OpenError *error = 0);
    bool close();
    bool isopen();

    /** Get explanation about last error */
    string getReason() const {return m_reason;}

    /** Return all possible stemmer names */
    static vector<string> getStemmerNames();

    /** Return existing stemming databases */
    vector<string> getStemLangs();

    /** Check if index stores the documents' texts. Only valid after open */
    bool storesDocText();
    
    /** Test word for spelling correction candidate: not too long, no 
     * special chars... 
     * @param with_aspell test for use with aspell, else for xapian speller
     */
    static bool isSpellingCandidate(const string& term, bool with_aspell=true);

    /** Return spelling suggestion */
    bool getSpellingSuggestions(const string& word, std::vector<std::string>& suggs);

    /* The next two, only for searchdata, should be somehow hidden */
    /* Return configured stop words */
    const StopList& getStopList() const {return m_stops;}
    /* Field name to prefix translation (ie: author -> 'A') */
    bool fieldToTraits(const string& fldname, const FieldTraits **ftpp, bool isquery = false);

    /* Query-related methods ************************************/

    /** Return total docs in db */
    int  docCnt(); 
    /** Return count of docs which have an occurrence of term */
    int termDocCnt(const string& term);
    /** Add extra Xapian database for querying. 
     * @param dir must point to something which can be passed as parameter 
     *      to a Xapian::Database constructor (directory or stub).
     */
    bool addQueryDb(const string &dir);
    /** Remove extra database. if dir == "", remove all. */
    bool rmQueryDb(const string &dir);
    /** Set the extra indexes to the input list. */
    bool setExtraQueryDbs(const std::vector<std::string>& dbs);

    /** Check if document comes from the main index (this is used to
        decide if we can update the index for it */
    bool fromMainIndex(const Doc& doc);

    /** Retrieve the stored doc text. This returns false if the index does not
        store raw text or other problems (discriminate with storesDocText(). 
        On success, the data is stored in doc.text
    */
    bool getDocRawText(Doc& doc);
    
    /** Retrieve an index designator for the document result. This is used 
     * by the GUI document history feature for remembering where a
     * doc comes from and allowing later retrieval (if the ext index
     * is still active...).
     */
    std::string whatIndexForResultDoc(const Doc& doc);
    
    /** Tell if directory seems to hold xapian db */
    static bool testDbDir(const string &dir, bool *stripped = 0);

    /** Return the index terms that match the input string
     * Expansion is performed either with either wildcard or regexp processing
     * Stem expansion is performed if lang is not empty 
     * 
     * @param typ_sens defines the kind of expansion: none, wildcard, 
     *    regexp or stemming. "none" may still expand case,
     *    diacritics and synonyms, depending on the casesens, diacsens and 
     *    synexp flags.
     * @param lang sets the stemming language(s). Can be a space-separated list
     * @param term is the term to expand
     * @param result is the main output
     * @param max defines the maximum result count
     * @param field if set, defines the field within with the expansion should
     *        be performed. Only used for wildcards and regexps, stemming is
     *        always global. If this is set, the resulting output terms 
     *        will be appropriately prefixed and the prefix value will be set 
     *        in the TermMatchResult header
     */
    enum MatchType {ET_NONE=0, ET_WILD=1, ET_REGEXP=2, ET_STEM=3, 
                    ET_DIACSENS=8, ET_CASESENS=16, ET_SYNEXP=32, ET_PATHELT=64};
    int matchTypeTp(int tp) {
        return tp & 7;
    }
    bool termMatch(int typ_sens, const string &lang, const string &term, 
                   TermMatchResult& result, int max = -1,
                   const string& field = "", vector<string> *multiwords = 0);
    bool dbStats(DbStats& stats, bool listFailed);
    /** Return min and max years for doc mod times in db */
    bool maxYearSpan(int *minyear, int *maxyear);
    /** Return all mime types in index. This can be different from the
        ones defined in the config because of 'file' command
        usage. Inserts the types at the end of the parameter */
    bool getAllDbMimeTypes(std::vector<std::string>&);

    /** Wildcard expansion specific to file names. Internal/sdata use only */
    bool filenameWildExp(const string& exp, vector<string>& names, int max);

    /** Set parameters for synthetic abstract generation */
    void setAbstractParams(int idxTrunc, int synthLen, int syntCtxLen);
    int getAbsCtxLen() const {
        return m_synthAbsWordCtxLen;
    }
    int getAbsLen() const {
        return m_synthAbsLen;
    }

    /** Get document for given udi and db index
     *
     * Used to retrieve ancestor documents.
     * @param udi The unique document identifier.
     * @param idxdoc A document from the same database as an opaque way to pass
     *   the database id (e.g.: when looking for parent in a multi-database 
     *   context).
     * @param[out] doc The output Recoll document.
     * @return True for success.
     */
    bool getDoc(const string &udi, const Doc& idxdoc, Doc &doc);

    /** Get document for given udi and index directory. 
     *
     * Used by the 'history' feature. This supposes that the extra db
     * is still active.
     * @param udi The unique document identifier.
     * @param dbdir The index directory, from storage, as returned by 
     *   whatIndexForResultDoc() at the time of the query. Can be
     *   empty to mean "main index" (allows the history to avoid
     *   storing the main dbdir value).
     * @param[out] doc The output Recoll document.
     * @return True for success.
     */
    bool getDoc(const string &udi, const std::string& dbdir, Doc &doc);

    /** Test if documents has sub-documents. 
     *
     * This can always be detected for file-level documents, using the
     * postlist for the parent term constructed with udi.
     *
     * For non file-level documents (e.g.: does an email inside an
     * mbox have attachments ?), detection is dependant on the filter
     * having set an appropriate flag at index time. Higher level code
     * can't detect it because the doc for the parent may have been
     * seen before any children. The flag is stored as a value in the
     * index.
     */
    bool hasSubDocs(const Doc &idoc);

    /** Get subdocuments of given document. 
     *
     * For file-level documents, these are all docs indexed by the
     * parent term built on idoc.udi. For embedded documents, the
     * parent doc is looked for, then its subdocs list is 
     * filtered using the idoc ipath as a prefix.
     */
    bool getSubDocs(const Doc& idoc, vector<Doc>& subdocs);

    /** Get container (top level file) document. 
     *
     * If the input is not a subdocument, this returns a copy of the input.
     */
    bool getContainerDoc(const Doc &idoc, Doc& ctdoc);
    
    /** Get duplicates (md5) of document */
    bool docDups(const Doc& idoc, std::vector<Doc>& odocs);

    /* The following are mainly for the aspell module */
    /** Whole term list walking. */
    TermIter *termWalkOpen();
    bool termWalkNext(TermIter *, string &term);
    void termWalkClose(TermIter *);
    /** Test term existence */
    bool termExists(const string& term);
    /** Test if terms stem to different roots. */
    bool stemDiffers(const string& lang, const string& term, 
                     const string& base);

    const RclConfig *getConf() {return m_config;}

    /** Test if the db entry for the given udi is up to date.
     *
     * This is done by comparing the input and stored sigs. This is
     * used both when indexing and querying (before opening a document 
     * using stale info).
     *
     * **This assumes that the udi pertains to the main index (idxi==0).**
     *
     * Side-effect when the db is writeable and the document up to
     * date: set the existence flag for the file document and all
     * subdocs if any (for later use by 'purge()')
     *
     * @param udi Unique Document Identifier (as chosen by indexer).
     * @param sig New signature (as computed by indexer).
     * @param xdocid[output] Non-zero if doc existed. Should be considered 
     *    as opaque, to be used for a possible later call to setExistingFlags()
     *    Note that if inplaceReset is set, the return value is non-zero but not
     *    an actual docid, it's only used as a flag in this case.
     * @param osig[output] old signature.
     */
    bool needUpdate(const string &udi, const string& sig, 
                    unsigned int *xdocid = 0, std::string *osig = 0);

    /** 
        Activate the "in place reset" mode where all documents are
        considered as needing update. This is a global/per-process
        option, and can't be reset. It should be set at the start of
        the indexing pass. 2012-10: no idea why this is done this way...
    */
    static void setInPlaceReset() {o_inPlaceReset = true;}

    /** Flush interval get/set. This is used by the first indexing
        pass to override the config value and flush more rapidly
        initially so that the user can quickly play with queries */
    int getFlushMb() {
        return  m_flushMb;
    }
    void setFlushMb(int mb) {
        m_flushMb = mb;
    }

    // Use empty fn for no synonyms
    bool setSynGroupsFile(const std::string& fn);
    const SynGroups& getSynGroups() {return m_syngroups;}
    
    class Native;
    friend class Native;
//    friend class DbW::NativeW;

    /* This has to be public for access by embedded Query::Native */
    Native *m_ndb{nullptr};
    
public:
    const RclConfig *m_config;
    string     m_reason; // Error explanation

    // Xapian directories for additional databases to query
    vector<string> m_extraDbs;
    OpenMode m_mode{Db::DbRO};

    // Synonym groups. There is no strict reason that this has to be
    // an Rcl::Db member, as it is only used when building each It
    // could be a SearchData member, or even a parameter to
    // Query::setQuery(). Otoh, building the syngroups structure from
    // a file may be expensive and it's unlikely to change with every
    // query, so it makes sense to cache it, and Rcl::Db is not a bad
    // place for this.
    SynGroups m_syngroups;

    // Aspell object if needed
    Aspell *m_aspell{nullptr};

    /***************
     * Parameters cached out of the configuration files. Logically const 
     * after init */
    // Stop terms: those don't get indexed.
    StopList m_stops;
    // Truncation length for stored meta fields
    int         m_idxMetaStoredLen{150};
    // This is how long an abstract we keep or build from beginning of
    // text when indexing. It only has an influence on the size of the
    // db as we are free to shorten it again when displaying
    int          m_idxAbsTruncLen{250};
    // Document text truncation length
    int          m_idxTextTruncateLen{0};
    // This is the size of the abstract that we synthetize out of query
    // term contexts at *query time*
    int          m_synthAbsLen{250};
    // This is how many words (context size) we keep around query terms
    // when building the abstract
    int          m_synthAbsWordCtxLen{4};
    // Flush threshold. Megabytes of text indexed before we flush.
    int          m_flushMb{-1};
    // Maximum file system occupation percentage
    int          m_maxFsOccupPc{0};
    // Database directory
    string       m_basedir;
    // When this is set, all documents are considered as needing a reindex.
    // This implements an alternative to just erasing the index before 
    // beginning, with the advantage that, for small index formats updates, 
    // between releases the index remains available while being recreated.
    static bool o_inPlaceReset;
    /******* End logical constnesss */

    // Internal form of close, can be called during destruction
    bool i_close(bool final);
    virtual void i_setExistingFlags(const string&, unsigned int) {}
    // Reinitialize when adding/removing additional dbs
    bool adjustdbs(); 
    bool idxTermMatch(int typ_sens, const string &lang, const string &term, 
                      TermMatchResult& result, int max = -1, 
                      const string& field = cstr_null);

    bool docExists(const string& uniterm);

    bool getDoc(const std::string& udi, int idxi, Doc& doc);

};

// This has to go somewhere, and as it needs the Xapian version, this is
// the most reasonable place.
string version_string();

extern const string pathelt_prefix;
extern const string mimetype_prefix;
extern const string unsplitFilenameFieldName;
extern string start_of_field_term;
extern string end_of_field_term;

} // namespace Rcl

#endif /* _DB_H_INCLUDED_ */
