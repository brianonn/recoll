/* Copyright (C) 2004-2018 J.F.Dockes
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
#include "autoconfig.h"

#include <stdio.h>
#include <cstring>
#include <exception>
#include "safeunistd.h"
#include <math.h>
#include <time.h>

#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <fstream>

using namespace std;

#include "xapian.h"

#include "rclconfig.h"
#define LOGGER_LOCAL_LOGINC 2
#include "log.h"
#include "rcldb.h"
#include "rcldb_p.h"
#include "stemdb.h"
#include "textsplit.h"
#include "transcode.h"
#include "unacpp.h"
#include "conftree.h"
#include "pathut.h"
#include "rclutil.h"
#include "smallut.h"
#include "chrono.h"
#include "searchdata.h"
#include "rclquery.h"
#include "rclquery_p.h"
#include "rclvalues.h"
#include "md5ut.h"
#include "cancelcheck.h"
#include "termproc.h"
#include "expansiondbs.h"
#include "rclinit.h"
#include "internfile.h"
#include "utf8fn.h"
#include "wipedir.h"
#ifdef RCL_USE_ASPELL
#include "rclaspell.h"
#endif
#include "zlibut.h"
#include "idxstatus.h"

namespace Rcl {

// Some prefixes that we could get from the fields file, but are not going
// to ever change.
const string fileext_prefix = "XE";
const string mimetype_prefix = "T";
const string xapday_prefix = "D";
const string xapmonth_prefix = "M";
const string xapyear_prefix = "Y";
const string pathelt_prefix = "XP";
const string parent_prefix("F");
const string udi_prefix("Q");
// Special term to mark documents with children.
const string has_children_term("XXC/");
const string unsplitfilename_prefix = "XSFS";
// Synthetic abstract marker (to discriminate from abstract actually
// found in document)
const string cstr_syntAbs("?!#@");
const string cstr_md5empty("d41d8cd98f00b204e9800998ecf8427e");

// Field name for the unsplit file name. Has to exist in the field file 
// because of usage in termmatch()
const string unsplitFilenameFieldName = "rclUnsplitFN";

// Special terms to mark begin/end of field (for anchored searches), and
// page breaks
string start_of_field_term;
string end_of_field_term;

string version_string(){
    return string("Recoll ") + string(PACKAGE_VERSION) + string(" + Xapian ") +
        string(Xapian::version_string());
}

Db::Native::Native(Db *db) 
    : m_rcldb(db)
{ 
    LOGDEB1("Native::Native: me " << this << "\n");
}

Db::Native::~Native() 
{ 
    LOGDEB1("Native::~Native: me " << this << "\n");
}

void Db::Native::storesDocText(Xapian::Database& db)
{
    string desc = db.get_metadata(cstr_RCL_IDX_DESCRIPTOR_KEY);
    ConfSimple cf(desc, 1);
    string val;
    m_storetext = false;
    if (cf.get("storetext", val) && stringToBool(val)) {
        m_storetext = true;
    }
    LOGDEB("Db:: index " << (m_storetext?"stores":"does not store") <<
           " document text\n");
}

void Db::Native::openRead(const string& dir)
{
    m_iswritable = false;
    xrdb = Xapian::Database(dir);
    storesDocText(xrdb);
}

/* See comment in class declaration: return all subdocuments of a
 * document given by its unique id. */
bool Db::Native::subDocs(const string &udi, int idxi, 
                         vector<Xapian::docid>& docids) 
{
    LOGDEB2("subDocs: [" << udi << "]\n");
    string pterm = make_parentterm(udi);
    vector<Xapian::docid> candidates;
    XAPTRY(docids.clear();
           candidates.insert(candidates.begin(), xrdb.postlist_begin(pterm), 
                             xrdb.postlist_end(pterm)),
           xrdb, m_rcldb->m_reason);
    if (!m_rcldb->m_reason.empty()) {
        LOGERR("Rcl::Db::subDocs: " << m_rcldb->m_reason << "\n");
        return false;
    } else {
        for (unsigned int i = 0; i < candidates.size(); i++) {
            if (whatDbIdx(candidates[i]) == (size_t)idxi) {
                docids.push_back(candidates[i]);
            }
        }
        LOGDEB0("Db::Native::subDocs: returning " << docids.size() << " ids\n");
        return true;
    }
}

bool Db::Native::xdocToUdi(Xapian::Document& xdoc, string &udi)
{
    Xapian::TermIterator xit;
    XAPTRY(xit = xdoc.termlist_begin();
           xit.skip_to(wrap_prefix(udi_prefix)),
           xrdb, m_rcldb->m_reason);
    if (!m_rcldb->m_reason.empty()) {
        LOGERR("xdocToUdi: xapian error: " << m_rcldb->m_reason << "\n");
        return false;
    }
    if (xit != xdoc.termlist_end()) {
        udi = *xit;
        if (!udi.empty()) {
            udi = udi.substr(wrap_prefix(udi_prefix).size());
            return true;
        }
    }
    return false;
}

// Check if doc given by udi is indexed by term
bool Db::Native::hasTerm(const string& udi, int idxi, const string& term)
{
    LOGDEB2("Native::hasTerm: udi [" << udi << "] term [" << term << "]\n");
    Xapian::Document xdoc;
    if (getDoc(udi, idxi, xdoc)) {
        Xapian::TermIterator xit;
        XAPTRY(xit = xdoc.termlist_begin();
               xit.skip_to(term);,
               xrdb, m_rcldb->m_reason);
        if (!m_rcldb->m_reason.empty()) {
            LOGERR("Rcl::Native::hasTerm: " << m_rcldb->m_reason << "\n");
            return false;
        }
        if (xit != xdoc.termlist_end() && !term.compare(*xit)) {
            return true;
        }
    }
    return false;
}

// Retrieve Xapian document, given udi. There may be several identical udis
// if we are using multiple indexes.
Xapian::docid Db::Native::getDoc(const string& udi, int idxi, Xapian::Document& xdoc)
{
    string uniterm = make_uniterm(udi);
    for (int tries = 0; tries < 2; tries++) {
        try {
            Xapian::PostingIterator docid;
            for (docid = xrdb.postlist_begin(uniterm); 
                 docid != xrdb.postlist_end(uniterm); docid++) {
                xdoc = xrdb.get_document(*docid);
                if (whatDbIdx(*docid) == (size_t)idxi)
                    return *docid;
            }
            // Udi not in Db.
            return 0;
        } catch (const Xapian::DatabaseModifiedError &e) {
            m_rcldb->m_reason = e.get_msg();
            xrdb.reopen();
            continue;
        } XCATCHERROR(m_rcldb->m_reason);
        break;
    }
    LOGERR("Db::Native::getDoc: Xapian error: " << m_rcldb->m_reason << "\n");
    return 0;
}

// Turn data record from db into document fields
bool Db::Native::dbDataToRclDoc(Xapian::docid docid, std::string &data, Doc &doc, bool fetchtext)
{
    LOGDEB2("Db::dbDataToRclDoc: data:\n" << data << "\n");
    ConfSimple parms(data);
    if (!parms.ok())
        return false;

    doc.xdocid = docid;
    doc.haspages = hasPages(docid);

    // Compute what index this comes from, and check for path translations
    string dbdir = m_rcldb->m_basedir;
    doc.idxi = 0;
    if (!m_rcldb->m_extraDbs.empty()) {
        int idxi = int(whatDbIdx(docid));

        // idxi is in [0, extraDbs.size()]. 0 is for the main index,
        // idxi-1 indexes into the additional dbs array.
        if (idxi) {
            dbdir = m_rcldb->m_extraDbs[idxi - 1];
            doc.idxi = idxi;
        }
    }
    parms.get(Doc::keyurl, doc.idxurl);
    doc.url = doc.idxurl;
    m_rcldb->m_config->urlrewrite(dbdir, doc.url);
    if (!doc.url.compare(doc.idxurl))
        doc.idxurl.clear();

    // Special cases:
    parms.get(Doc::keytp, doc.mimetype);
    parms.get(Doc::keyfmt, doc.fmtime);
    parms.get(Doc::keydmt, doc.dmtime);
    parms.get(Doc::keyoc, doc.origcharset);
    parms.get(cstr_caption, doc.meta[Doc::keytt]);

    parms.get(Doc::keyabs, doc.meta[Doc::keyabs]);
    // Possibly remove synthetic abstract indicator (if it's there, we
    // used to index the beginning of the text as abstract).
    doc.syntabs = false;
    if (doc.meta[Doc::keyabs].find(cstr_syntAbs) == 0) {
        doc.meta[Doc::keyabs] = 
            doc.meta[Doc::keyabs].substr(cstr_syntAbs.length());
        doc.syntabs = true;
    }
    parms.get(Doc::keyipt, doc.ipath);
    parms.get(Doc::keypcs, doc.pcbytes);
    parms.get(Doc::keyfs, doc.fbytes);
    parms.get(Doc::keyds, doc.dbytes);
    parms.get(Doc::keysig, doc.sig);

    // Normal key/value pairs:
    vector<string> keys = parms.getNames(string());
    for (vector<string>::const_iterator it = keys.begin(); 
         it != keys.end(); it++) {
        if (doc.meta.find(*it) == doc.meta.end())
            parms.get(*it, doc.meta[*it]);
    }
    doc.meta[Doc::keyurl] = doc.url;
    doc.meta[Doc::keymt] = doc.dmtime.empty() ? doc.fmtime : doc.dmtime;
    if (fetchtext) {
        getRawText(docid, doc.text);
    }
    return true;
}

bool Db::Native::hasPages(Xapian::docid docid)
{
    string ermsg;
    Xapian::PositionIterator pos;
    XAPTRY(pos = xrdb.positionlist_begin(docid, page_break_term); 
           if (pos != xrdb.positionlist_end(docid, page_break_term)) {
               return true;
           },
           xrdb, ermsg);
    if (!ermsg.empty()) {
        LOGERR("Db::Native::hasPages: xapian error: " << ermsg << "\n");
    }
    return false;
}

// Return the positions list for the page break term
bool Db::Native::getPagePositions(Xapian::docid docid, vector<int>& vpos)
{
    vpos.clear();
    // Need to retrieve the document record to check for multiple page breaks
    // that we store there for lack of better place
    map<int, int> mbreaksmap;
    try {
        Xapian::Document xdoc = xrdb.get_document(docid);
        string data = xdoc.get_data();
        Doc doc;
        string mbreaks;
        if (dbDataToRclDoc(docid, data, doc) && 
            doc.getmeta(cstr_mbreaks, &mbreaks)) {
            vector<string> values;
            stringToTokens(mbreaks, values, ",");
            for (unsigned int i = 0; i < values.size() - 1; i += 2) {
                int pos  = atoi(values[i].c_str()) + baseTextPosition;
                int incr = atoi(values[i+1].c_str());
                mbreaksmap[pos] = incr;
            }
        }
    } catch (...) {
    }

    string qterm = page_break_term;
    Xapian::PositionIterator pos;
    try {
        for (pos = xrdb.positionlist_begin(docid, qterm); 
             pos != xrdb.positionlist_end(docid, qterm); pos++) {
            int ipos = *pos;
            if (ipos < int(baseTextPosition)) {
                LOGDEB("getPagePositions: got page position " << ipos
                       << " not in body\n");
                // Not in text body. Strange...
                continue;
            }
            map<int, int>::iterator it = mbreaksmap.find(ipos);
            if (it != mbreaksmap.end()) {
                LOGDEB1("getPagePositions: found multibreak at " << ipos <<
                        " incr " << it->second << "\n");
                for (int i = 0 ; i < it->second; i++) 
                    vpos.push_back(ipos);
            }
            vpos.push_back(ipos);
        } 
    } catch (...) {
        // Term does not occur. No problem.
    }
    return true;
}

int Db::Native::getPageNumberForPosition(const vector<int>& pbreaks, int pos)
{
    if (pos < int(baseTextPosition)) // Not in text body
        return -1;
    vector<int>::const_iterator it = 
        upper_bound(pbreaks.begin(), pbreaks.end(), pos);
    return int(it - pbreaks.begin() + 1);
}

bool Db::Native::getRawText(Xapian::docid docid_combined, string& rawtext)
{
    if (!m_storetext) {
        LOGDEB("Db::Native::getRawText: document text not stored in index\n");
        return false;
    }

    // Xapian get_metadata only works on a single index (else of
    // course, unicity of keys can't be ensured). When using multiple
    // indexes, we need to open the right one.
    size_t dbidx = whatDbIdx(docid_combined);
    Xapian::docid docid = whatDbDocid(docid_combined);
    string reason;
    if (dbidx != 0) {
        Xapian::Database db(m_rcldb->m_extraDbs[dbidx-1]);
        XAPTRY(rawtext = db.get_metadata(rawtextMetaKey(docid)), db, reason);
    } else {
        XAPTRY(rawtext = xrdb.get_metadata(rawtextMetaKey(docid)), xrdb, reason);
    }
    if (!reason.empty()) {
        LOGERR("Rcl::Db::getRawText: could not get value: " << reason << endl);
        return false;
    }
    if (rawtext.empty()) {
        return true;
    }
    ZLibUtBuf cbuf;
    inflateToBuf(rawtext.c_str(), rawtext.size(), cbuf);
    rawtext.assign(cbuf.getBuf(), cbuf.getCnt());
    return true;
}

/* Rcl::Db methods ///////////////////////////////// */

bool Db::o_inPlaceReset;

Db::Db(const RclConfig *cfp)
{
    m_config = new RclConfig(*cfp);
    m_config->getConfParam("maxfsoccuppc", &m_maxFsOccupPc);
    m_config->getConfParam("idxflushmb", &m_flushMb);
    m_config->getConfParam("idxmetastoredlen", &m_idxMetaStoredLen);
    m_config->getConfParam("idxtexttruncatelen", &m_idxTextTruncateLen);
    if (start_of_field_term.empty()) {
        if (o_index_stripchars) {
            start_of_field_term = "XXST";
            end_of_field_term = "XXND";
        } else {
            start_of_field_term = "XXST/";
            end_of_field_term = "XXND/";
        }
    }
    m_ndb = new Native(this);
}

Db::~Db()
{
    LOGDEB1("Db::~Db\n");
    if (nullptr == m_ndb)
        return;
    LOGDEB("Db::~Db: isopen " << m_ndb->m_isopen << " m_iswritable " <<
           m_ndb->m_iswritable << "\n");
    i_close(true);
#ifdef RCL_USE_ASPELL
    delete m_aspell;
#endif
    delete m_config;
}

vector<string> Db::getStemmerNames()
{
    vector<string> res;
    stringToStrings(Xapian::Stem::get_available_languages(), res);
    return res;
}

bool Db::open(OpenMode mode, OpenError *error)
{
    if (error)
        *error = DbOpenMainDb;

    if (m_ndb == 0 || m_config == 0) {
        m_reason = "Null configuration or Xapian Db";
        return false;
    }
    LOGDEB("Db::open: m_isopen " << m_ndb->m_isopen << " m_iswritable " <<
           m_ndb->m_iswritable << " mode " << mode << "\n");

    if (m_ndb->m_isopen) {
        // We used to return an error here but I see no reason to
        if (!close())
            return false;
    }
    if (!m_config->getStopfile().empty())
        m_stops.setFile(m_config->getStopfile());

    if (isWriteMode(mode)) {
        // Check for an index-time synonyms file. We use this to
        // generate multiword terms for multiword synonyms
        string synfile = m_config->getIdxSynGroupsFile();
        if (path_exists(synfile)) {
            setSynGroupsFile(synfile);
        }
    }
    
    string dir = m_config->getDbDir();
    string ermsg;
    try {
        if (isWriteMode(mode)) {
            m_ndb->openWrite(dir, mode);
        } else {
            m_ndb->openRead(dir);
            for (auto& db : m_extraDbs) {
                if (error)
                    *error = DbOpenExtraDb;
                LOGDEB("Db::Open: adding query db [" << &db << "]\n");
                // An error here used to be non-fatal (1.13 and older)
                // but I can't see why
                m_ndb->xrdb.add_database(Xapian::Database(db));
            }
        }
        if (error)
            *error = DbOpenMainDb;

        // Check index format version. Must not try to check a just created or
        // truncated db
        if (mode != DbTrunc && m_ndb->xrdb.get_doccount() > 0) {
            string version = m_ndb->xrdb.get_metadata(cstr_RCL_IDX_VERSION_KEY);
            if (version.compare(cstr_RCL_IDX_VERSION)) {
                m_ndb->m_noversionwrite = true;
                LOGERR("Rcl::Db::open: file index [" << version <<
                       "], software [" << cstr_RCL_IDX_VERSION << "]\n");
                throw Xapian::DatabaseError("Recoll index version mismatch",
                                            "", "");
            }
        }
        m_mode = mode;
        m_ndb->m_isopen = true;
        m_basedir = dir;
        if (error)
            *error = DbOpenNoError;
        return true;
    } XCATCHERROR(ermsg);

    m_reason = ermsg;
    LOGERR("Db::open: exception while opening [" <<dir<< "]: " << ermsg << "\n");
    return false;
}

bool Db::storesDocText()
{
    if (!m_ndb || !m_ndb->m_isopen) {
        LOGERR("Db::storesDocText: called on non-opened db\n");
        return false;
    }
    return m_ndb->m_storetext;
}

bool Db::getDocRawText(Doc& doc)
{
    if (!m_ndb || !m_ndb->m_isopen) {
        LOGERR("Db::getDocRawText: called on non-opened db\n");
        return false;
    }
    return m_ndb->getRawText(doc.xdocid, doc.text);
}

// Note: xapian has no close call, we delete and recreate the db
bool Db::close()
{
    LOGDEB1("Db::close()\n");
    return i_close(false);
}

bool Db::createNative()
{
    m_ndb = new Native(this);
    return nullptr != m_ndb;
}

bool Db::i_close(bool final)
{
    if (nullptr == m_ndb)
        return false;
    LOGDEB("Db::i_close(" << final << "): m_isopen " << m_ndb->m_isopen <<
           " m_iswritable " << m_ndb->m_iswritable << "\n");
    if (!m_ndb->m_isopen && !final) 
        return true;

    bool w = m_ndb->m_iswritable;
    LOGDEB("Db::i_close: writable: " << m_ndb->m_iswritable << "\n");
    string ermsg;
    try {
        if (w)
            m_ndb->closeWrite();
        deleteZ(m_ndb);
        if (w)
            LOGDEB("Rcl::Db:close() xapian close done.\n");
        if (final) {
            return true;
        }
        if (createNative()) {
            return true;
        }
        LOGERR("Rcl::Db::close(): cant recreate db object\n");
        return false;
    } XCATCHERROR(ermsg);
    LOGERR("Db:close: exception while deleting db: " << ermsg << "\n");
    return false;
}

// Test if doc given by udi has changed since last indexed (test sigs)
bool Db::needUpdate(const string &udi, const string& sig, 
                    unsigned int *docidp, string *osigp)
{
    if (m_ndb == 0)
        return false;

    if (osigp)
        osigp->clear();
    if (docidp)
        *docidp = 0;

    // If we are doing an in place or full reset, no need to test.
    if (o_inPlaceReset || m_mode == DbTrunc) {
        // For in place reset, pretend the doc existed, to enable
        // subdoc purge. The value is only used as a boolean in this case.
        if (docidp && o_inPlaceReset) {
            *docidp = -1;
        }
        return true;
    }

    string uniterm = make_uniterm(udi);
    string ermsg;

#ifdef IDX_THREADS
    // Need to protect against interaction with the doc update/insert
    // thread which also updates the existence map, and even multiple
    // accesses to the readonly Xapian::Database are not allowed
    // anyway
    std::unique_lock<std::mutex> lock(m_ndb->m_mutex);
#endif

    // Try to find the document indexed by the uniterm. 
    Xapian::PostingIterator docid;
    XAPTRY(docid = m_ndb->xrdb.postlist_begin(uniterm), m_ndb->xrdb, m_reason);
    if (!m_reason.empty()) {
        LOGERR("Db::needUpdate: xapian::postlist_begin failed: " << m_reason << "\n");
        return false;
    }
    if (docid == m_ndb->xrdb.postlist_end(uniterm)) {
        // No document exists with this path: we do need update
        LOGDEB("Db::needUpdate:yes (new): [" << uniterm << "]\n");
        return true;
    }
    Xapian::Document xdoc;
    XAPTRY(xdoc = m_ndb->xrdb.get_document(*docid), m_ndb->xrdb, m_reason);
    if (!m_reason.empty()) {
        LOGERR("Db::needUpdate: get_document error: " << m_reason << "\n");
        return true;
    }

    if (docidp) {
        *docidp = *docid;
    }

    // Retrieve old file/doc signature from value
    string osig;
    XAPTRY(osig = xdoc.get_value(VALUE_SIG), m_ndb->xrdb, m_reason);
    if (!m_reason.empty()) {
        LOGERR("Db::needUpdate: get_value error: " << m_reason << "\n");
        return true;
    }
    LOGDEB2("Db::needUpdate: oldsig [" << osig << "] new [" << sig << "]\n");

    if (osigp) {
        *osigp = osig;
    }

    // Compare new/old sig
    if (sig != osig) {
        LOGDEB("Db::needUpdate:yes: olsig [" << osig << "] new [" << sig <<
               "] [" << uniterm << "]\n");
        // Db is not up to date. Let's index the file
        return true;
    }

    // Up to date. Set the existance flags in the map for the doc and
    // its subdocs.
    LOGDEB("Db::needUpdate:no: [" << uniterm << "]\n");
    i_setExistingFlags(udi, *docid);
    return false;
}

int Db::docCnt()
{
    int res = -1;
    if (!m_ndb || !m_ndb->m_isopen)
        return -1;

    XAPTRY(res = m_ndb->xrdb.get_doccount(), m_ndb->xrdb, m_reason);

    if (!m_reason.empty()) {
        LOGERR("Db::docCnt: got error: " << m_reason << "\n");
        return -1;
    }
    return res;
}

int Db::termDocCnt(const string& _term)
{
    int res = -1;
    if (!m_ndb || !m_ndb->m_isopen)
        return -1;

    string term = _term;
    if (o_index_stripchars)
        if (!unacmaybefold(_term, term, "UTF-8", UNACOP_UNACFOLD)) {
            LOGINFO("Db::termDocCnt: unac failed for [" << _term << "]\n");
            return 0;
        }

    if (m_stops.isStop(term)) {
        LOGDEB1("Db::termDocCnt [" << term << "] in stop list\n");
        return 0;
    }

    XAPTRY(res = m_ndb->xrdb.get_termfreq(term), m_ndb->xrdb, m_reason);

    if (!m_reason.empty()) {
        LOGERR("Db::termDocCnt: got error: " << m_reason << "\n");
        return -1;
    }
    return res;
}

// Reopen the db with a changed list of additional dbs
bool Db::adjustdbs()
{
    if (m_mode != DbRO) {
        LOGERR("Db::adjustdbs: mode not RO\n");
        return false;
    }
    if (m_ndb && m_ndb->m_isopen) {
        if (!close())
            return false;
        if (!open(m_mode)) {
            return false;
        }
    }
    return true;
}

// Set the extra indexes to the input list.
bool Db::setExtraQueryDbs(const std::vector<std::string>& dbs)
{
    LOGDEB0("Db::setExtraQueryDbs: ndb " << m_ndb << " iswritable " <<
            ((m_ndb)?m_ndb->m_iswritable:0) << " dbs [" <<
            stringsToString(dbs) << "]\n");
    if (!m_ndb) {
        return false;
    }
    if (m_ndb->m_iswritable) {
        return false;
    }
    m_extraDbs.clear();
    for (const auto& dir : dbs) {
        m_extraDbs.push_back(path_canon(dir));
    }
    return adjustdbs();
}

bool Db::addQueryDb(const string &_dir) 
{
    string dir = _dir;
    LOGDEB0("Db::addQueryDb: ndb " << m_ndb << " iswritable " <<
            ((m_ndb)?m_ndb->m_iswritable:0) << " db [" << dir << "]\n");
    if (!m_ndb)
        return false;
    if (m_ndb->m_iswritable)
        return false;
    dir = path_canon(dir);
    if (find(m_extraDbs.begin(), m_extraDbs.end(), dir) == m_extraDbs.end()) {
        m_extraDbs.push_back(dir);
    }
    return adjustdbs();
}

bool Db::rmQueryDb(const string &dir)
{
    if (!m_ndb)
        return false;
    if (m_ndb->m_iswritable)
        return false;
    if (dir.empty()) {
        m_extraDbs.clear();
    } else {
        vector<string>::iterator it = find(m_extraDbs.begin(), 
                                           m_extraDbs.end(), dir);
        if (it != m_extraDbs.end()) {
            m_extraDbs.erase(it);
        }
    }
    return adjustdbs();
}

// Determining what index a doc result comes from is based on the
// modulo of the docid against the db count. Ref:
// http://trac.xapian.org/wiki/FAQ/MultiDatabaseDocumentID
bool Db::fromMainIndex(const Doc& doc)
{
    return m_ndb->whatDbIdx(doc.xdocid) == 0;
}

std::string Db::whatIndexForResultDoc(const Doc& doc)
{
    size_t idx = m_ndb->whatDbIdx(doc.xdocid);
    if (idx == (size_t)-1) {
        LOGERR("whatIndexForResultDoc: whatDbIdx returned -1 for " <<
               doc.xdocid << endl);
        return string();
    }
    // idx is [0..m_extraDbs.size()] 0 is for the main index, else
    // idx-1 indexes into m_extraDbs
    if (idx == 0) {
        return m_basedir;
    } else {
        return m_extraDbs[idx-1];
    }
}

size_t Db::Native::whatDbIdx(Xapian::docid id)
{
    LOGDEB1("Db::whatDbIdx: xdocid " << id << ", " <<
            m_rcldb->m_extraDbs.size() << " extraDbs\n");
    if (id == 0) 
        return (size_t)-1;
    if (m_rcldb->m_extraDbs.size() == 0)
        return 0;
    return (id - 1) % (m_rcldb->m_extraDbs.size() + 1);
}

// Return the docid inside the non-combined index
Xapian::docid Db::Native::whatDbDocid(Xapian::docid docid_combined)
{
    if (m_rcldb->m_extraDbs.size() == 0)
        return docid_combined;
    return (docid_combined - 1) / (m_rcldb->m_extraDbs.size() + 1) + 1;
}

bool Db::testDbDir(const string &dir, bool *stripped_p)
{
    string aerr;
    bool mstripped = true;
    LOGDEB("Db::testDbDir: [" << dir << "]\n");
    try {
        Xapian::Database db(dir);
        // If the prefix for mimetype is wrapped, it's an unstripped
        // index. T has been in use in recoll since the beginning and
        // all documents have a T field (possibly empty).
        Xapian::TermIterator term = db.allterms_begin(":T:");
        if (term == db.allterms_end()) {
            mstripped = true;
        } else {
            mstripped = false;
        }
        LOGDEB("testDbDir: " << dir << " is a " <<
               (mstripped ? "stripped" : "raw") << " index\n");
    } XCATCHERROR(aerr);
    if (!aerr.empty()) {
        LOGERR("Db::Open: error while trying to open database from [" <<
               dir << "]: " << aerr << "\n");
        return false;
    }
    if (stripped_p) 
        *stripped_p = mstripped;

    return true;
}

bool Db::isopen()
{
    if (m_ndb == 0)
        return false;
    return m_ndb->m_isopen;
}

// Try to translate field specification into field prefix. 
bool Db::fieldToTraits(const string& fld, const FieldTraits **ftpp, bool isquery)
{
    if (m_config && m_config->getFieldTraits(fld, ftpp, isquery))
        return true;

    *ftpp = 0;
    return false;
}



// At the moment, we normally use the Xapian speller for Katakana and
// aspell for everything else
bool Db::getSpellingSuggestions(const string& word, vector<string>& suggs)
{
    LOGDEB("Db::getSpellingSuggestions:[" << word << "]\n");
    suggs.clear();
    if (nullptr == m_ndb) {
        return false;
    }

    string term = word;

    if (isSpellingCandidate(term, true)) {
        // Term is candidate for aspell processing
#ifdef RCL_USE_ASPELL
        bool noaspell = false;
        m_config->getConfParam("noaspell", &noaspell);
        if (noaspell) {
            return false;
        }
        if (nullptr == m_aspell) {
            m_aspell = new Aspell(m_config);
            if (m_aspell) {
                string reason;
                m_aspell->init(reason);
                if (!m_aspell->ok()) {
                    LOGDEB("Aspell speller init failed: " << reason << endl);
                    delete m_aspell;
                    m_aspell = 0;
                }
            }
        }

        if (nullptr == m_aspell) {
            LOGERR("Db::getSpellingSuggestions: aspell not initialized\n");
            return false;
        }

        string reason;
        if (!m_aspell->suggest(*this, term, suggs, reason)) {
            LOGERR("Db::getSpellingSuggestions: aspell failed: " << reason << "\n");
            return false;
        }
#endif
    } else {
#ifdef TESTING_XAPIAN_SPELL
        // Was not aspell candidate (e.g.: katakana). Maybe use Xapian
        // speller?
        if (isSpellingCandidate(term, false)) {
            if (!o_index_stripchars) {
                if (!unacmaybefold(word, term, "UTF-8", UNACOP_UNACFOLD)) {
                    LOGINFO("Db::getSpelling: unac failed for [" << word <<
                            "]\n");
                    return false;
                }
            }
            string sugg = m_ndb->xrdb.get_spelling_suggestion(term);
            if (!sugg.empty()) {
                suggs.push_back(sugg);
            }
        }
#endif
    }
    return true;
}

// Let our user set the parameters for abstract processing
void Db::setAbstractParams(int idxtrunc, int syntlen, int syntctxlen)
{
    LOGDEB1("Db::setAbstractParams: trunc " << idxtrunc << " syntlen " <<
            syntlen << " ctxlen " << syntctxlen << "\n");
    if (idxtrunc >= 0)
        m_idxAbsTruncLen = idxtrunc;
    if (syntlen > 0)
        m_synthAbsLen = syntlen;
    if (syntctxlen > 0)
        m_synthAbsWordCtxLen = syntctxlen;
}

bool Db::setSynGroupsFile(const string& fn)
{
    return m_syngroups.setfile(fn);
}
    

// Return existing stem db languages
vector<string> Db::getStemLangs()
{
    LOGDEB("Db::getStemLang\n");
    vector<string> langs;
    if (m_ndb == 0 || m_ndb->m_isopen == false)
        return langs;
    StemDb db(m_ndb->xrdb);
    db.getMembers(langs);
    return langs;
}


// Test for doc existence.
bool Db::docExists(const string& uniterm)
{
#ifdef IDX_THREADS
    // Need to protect read db against multiaccess. 
    std::unique_lock<std::mutex> lock(m_ndb->m_mutex);
#endif

    string ermsg;
    try {
        Xapian::PostingIterator docid = m_ndb->xrdb.postlist_begin(uniterm);
        if (docid == m_ndb->xrdb.postlist_end(uniterm)) {
            return false;
        } else {
            return true;
        }
    } XCATCHERROR(ermsg);
    if (!ermsg.empty()) {
        LOGERR("Db::docExists(" << uniterm << ") " << ermsg << "\n");
    }
    return false;
}

bool Db::dbStats(DbStats& res, bool listfailed)
{
    if (!m_ndb || !m_ndb->m_isopen)
        return false;
    Xapian::Database xdb = m_ndb->xrdb;

    XAPTRY(res.dbdoccount = xdb.get_doccount();
           res.dbavgdoclen = xdb.get_avlength();
           res.mindoclen = xdb.get_doclength_lower_bound();
           res.maxdoclen = xdb.get_doclength_upper_bound();
           , xdb, m_reason);
    if (!m_reason.empty())
        return false;
    if (!listfailed) {
        return true;
    }

    // listfailed is set : look for failed docs
    string ermsg;
    try {
        for (unsigned int docid = 1; docid < xdb.get_lastdocid(); docid++) {
            try {
                Xapian::Document doc = xdb.get_document(docid);
                string sig = doc.get_value(VALUE_SIG);
                if (sig.empty() || sig.back() != '+') {
                    continue;
                }
                string data = doc.get_data();
                ConfSimple parms(data);
                if (!parms.ok()) {
                } else {
                    string url, ipath;
                    parms.get(Doc::keyipt, ipath);
                    parms.get(Doc::keyurl, url);
                    // Turn to local url or not? It seems to make more
                    // sense to keep the original urls as seen by the
                    // indexer.
                    // m_config->urlrewrite(dbdir, url);
                    if (!ipath.empty()) {
                        url += " | " + ipath;
                    }
                    res.failedurls.push_back(url);
                }
            } catch (Xapian::DocNotFoundError) {
                continue;
            }
        }
    } XCATCHERROR(ermsg);
    if (!ermsg.empty()) {
        LOGERR("Db::dbStats: " << ermsg << "\n");
        return false;
    }
    return true;
}

// Retrieve document defined by Unique doc identifier. This is used
// by the GUI history feature and by open parent/getenclosing
// ! The return value is always true except for fatal errors. Document
//  existence should be tested by looking at doc.pc
bool Db::getDoc(const string &udi, const Doc& idxdoc, Doc &doc)
{
    LOGDEB1("Db:getDoc: [" << udi << "]\n");
    int idxi = idxdoc.idxi;
    return getDoc(udi, idxi, doc);
}

bool Db::getDoc(const string &udi, const std::string& dbdir, Doc &doc)
{
    LOGDEB1("Db::getDoc(udi, dbdir): (" << udi << ", " << dbdir << ")\n");
    int idxi = -1;
    if (dbdir.empty() || dbdir == m_basedir) {
        idxi = 0;
    } else {
        for (unsigned int i = 0; i < m_extraDbs.size(); i++) {
            if (dbdir == m_extraDbs[i]) {
                idxi = int(i + 1);
                break;
            }
        }
    }
    LOGDEB1("Db::getDoc(udi, dbdir): idxi: " << idxi << endl);
    if (idxi < 0) {
        LOGERR("Db::getDoc(udi, dbdir): dbdir not in current extra dbs\n");
        return false;
    }
    return getDoc(udi, idxi, doc);
}

bool Db::getDoc(const string& udi, int idxi, Doc& doc)
{
    // Initialize what we can in any case. If this is history, caller
    // will make partial display in case of error
    if (m_ndb == 0)
        return false;
    doc.meta[Rcl::Doc::keyrr] = "100%";
    doc.pc = 100;
    Xapian::Document xdoc;
    Xapian::docid docid;
    if (idxi >= 0 && (docid = m_ndb->getDoc(udi, idxi, xdoc))) {
        string data = xdoc.get_data();
        doc.meta[Rcl::Doc::keyudi] = udi;
        return m_ndb->dbDataToRclDoc(docid, data, doc);
    } else {
        // Document found in history no longer in the
        // database.  We return true (because their might be
        // other ok docs further) but indicate the error with
        // pc = -1
        doc.pc = -1;
        LOGINFO("Db:getDoc: no such doc in current index: [" << udi << "]\n");
        return true;
    }
}

bool Db::hasSubDocs(const Doc &idoc)
{
    if (m_ndb == 0)
        return false;
    string inudi;
    if (!idoc.getmeta(Doc::keyudi, &inudi) || inudi.empty()) {
        LOGERR("Db::hasSubDocs: no input udi or empty\n");
        return false;
    }
    LOGDEB1("Db::hasSubDocs: idxi " << idoc.idxi << " inudi [" <<inudi << "]\n");

    // Not sure why we perform both the subDocs() call and the test on
    // has_children. The former will return docs if the input is a
    // file-level document, but the latter should be true both in this
    // case and if the input is already a subdoc, so the first test
    // should be redundant. Does not hurt much in any case, to be
    // checked one day.
    vector<Xapian::docid> docids;
    if (!m_ndb->subDocs(inudi, idoc.idxi, docids)) {
        LOGDEB("Db::hasSubDocs: lower level subdocs failed\n");
        return false;
    }
    if (!docids.empty())
        return true;

    // Check if doc has an "has_children" term
    if (m_ndb->hasTerm(inudi, idoc.idxi, has_children_term))
        return true;
    return false;
}

// Retrieve all subdocuments of a given one, which may not be a file-level
// one (in which case, we have to retrieve this first, then filter the ipaths)
bool Db::getSubDocs(const Doc &idoc, vector<Doc>& subdocs)
{
    if (m_ndb == 0)
        return false;

    string inudi;
    if (!idoc.getmeta(Doc::keyudi, &inudi) || inudi.empty()) {
        LOGERR("Db::getSubDocs: no input udi or empty\n");
        return false;
    }

    string rootudi;
    string ipath = idoc.ipath;
    LOGDEB0("Db::getSubDocs: idxi " << idoc.idxi << " inudi [" << inudi <<
            "] ipath [" << ipath << "]\n");
    if (ipath.empty()) {
        // File-level doc. Use it as root
        rootudi = inudi;
    } else {
        // See if we have a parent term
        Xapian::Document xdoc;
        if (!m_ndb->getDoc(inudi, idoc.idxi, xdoc)) {
            LOGERR("Db::getSubDocs: can't get Xapian document\n");
            return false;
        }
        Xapian::TermIterator xit;
        XAPTRY(xit = xdoc.termlist_begin();
               xit.skip_to(wrap_prefix(parent_prefix)),
               m_ndb->xrdb, m_reason);
        if (!m_reason.empty()) {
            LOGERR("Db::getSubDocs: xapian error: " << m_reason << "\n");
            return false;
        }
        if (xit == xdoc.termlist_end() || get_prefix(*xit) != parent_prefix) {
            LOGERR("Db::getSubDocs: parent term not found\n");
            return false;
        }
        rootudi = strip_prefix(*xit);
    }

    LOGDEB("Db::getSubDocs: root: [" << rootudi << "]\n");

    // Retrieve all subdoc xapian ids for the root
    vector<Xapian::docid> docids;
    if (!m_ndb->subDocs(rootudi, idoc.idxi, docids)) {
        LOGDEB("Db::getSubDocs: lower level subdocs failed\n");
        return false;
    }

    // Retrieve doc, filter, and build output list
    for (int tries = 0; tries < 2; tries++) {
        try {
            for (vector<Xapian::docid>::const_iterator it = docids.begin();
                 it != docids.end(); it++) {
                Xapian::Document xdoc = m_ndb->xrdb.get_document(*it);
                string data = xdoc.get_data();
                string docudi;
                m_ndb->xdocToUdi(xdoc, docudi);
                Doc doc;
                doc.meta[Doc::keyudi] = docudi;
                doc.meta[Doc::keyrr] = "100%";
                doc.pc = 100;
                if (!m_ndb->dbDataToRclDoc(*it, data, doc)) {
                    LOGERR("Db::getSubDocs: doc conversion error\n");
                    return false;
                }
                if (ipath.empty() ||
                    FileInterner::ipathContains(ipath, doc.ipath)) {
                    subdocs.push_back(doc);
                }
            }
            return true;
        } catch (const Xapian::DatabaseModifiedError &e) {
            m_reason = e.get_msg();
            m_ndb->xrdb.reopen();
            continue;
        } XCATCHERROR(m_reason);
        break;
    }

    LOGERR("Db::getSubDocs: Xapian error: " << m_reason << "\n");
    return false;
}

bool Db::getContainerDoc(const Doc &idoc, Doc& ctdoc)
{
    if (m_ndb == 0)
        return false;

    string inudi;
    if (!idoc.getmeta(Doc::keyudi, &inudi) || inudi.empty()) {
        LOGERR("Db::getContainerDoc: no input udi or empty\n");
        return false;
    }

    string rootudi;
    string ipath = idoc.ipath;
    LOGDEB0("Db::getContainerDoc: idxi " << idoc.idxi << " inudi [" << inudi <<
            "] ipath [" << ipath << "]\n");
    if (ipath.empty()) {
        // File-level doc ??
        ctdoc = idoc;
        return true;
    } 
    // See if we have a parent term
    Xapian::Document xdoc;
    if (!m_ndb->getDoc(inudi, idoc.idxi, xdoc)) {
        LOGERR("Db::getContainerDoc: can't get Xapian document\n");
        return false;
    }
    Xapian::TermIterator xit;
    XAPTRY(xit = xdoc.termlist_begin();
           xit.skip_to(wrap_prefix(parent_prefix)),
           m_ndb->xrdb, m_reason);
    if (!m_reason.empty()) {
        LOGERR("Db::getContainerDoc: xapian error: " << m_reason << "\n");
        return false;
    }
    if (xit == xdoc.termlist_end() || get_prefix(*xit) != parent_prefix) {
        LOGERR("Db::getContainerDoc: parent term not found\n");
        return false;
    }
    rootudi = strip_prefix(*xit);

    if (!getDoc(rootudi, idoc.idxi, ctdoc)) {
        LOGERR("Db::getContainerDoc: can't get container document\n");
        return false;
    }
    return true;
}


bool Db::isSpellingCandidate(const string& term, bool with_aspell)
{
    if (term.empty() || term.length() > 50 || has_prefix(term))
        return false;

    Utf8Iter u8i(term);
    if (with_aspell) {
        // If spelling with aspell, CJK scripts are not candidates
        if (TextSplit::isCJK(*u8i))
            return false;
    } else {
#ifdef TESTING_XAPIAN_SPELL
        // The Xapian speller (purely proximity-based) can be used
        // for Katakana (when split as words which is not always
        // completely feasible because of separator-less
        // compounds). Currently we don't try to use the Xapian
        // speller with other scripts with which it would be usable
        // in the absence of aspell (it would indeed be better
        // than nothing with e.g. european languages). This would
        // require a few more config variables, maybe one day.
        if (!TextSplit::isKATAKANA(*u8i)) {
            return false;
        }
#else
        return false;
#endif
    }
    if (term.find_first_of(" !\"#$%&()*+,-./0123456789:;<=>?@[\\]^_`{|}~") != string::npos)
        return false;
    return true;
}

} // End namespace Rcl
