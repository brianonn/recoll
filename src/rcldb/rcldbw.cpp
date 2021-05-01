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
#include "log.h"
#include "rclquery.h"
#include "rclquery_p.h"
#include "rcldbw.h"
#include "rcldbw_p.h"
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

static const int MB = 1024 * 1024;
static const string cstr_nc("\n\r\x0c\\");

namespace Rcl {

// The splitter breaks text into words and adds postings to the Xapian
// document. We use a single object to split all of the document
// fields and position jumps to separate fields
class TextSplitDb : public TextSplitP {
public:
    Xapian::Document &doc;   // Xapian document 
    // Base for document section. Gets large increment when we change
    // sections, to avoid cross-section proximity matches.
    Xapian::termpos basepos;
    // Current relative position. This is the remembered value from
    // the splitter callback. The term position is reset for each call
    // to text_to_words(), so that the last value of curpos is the
    // section size (last relative term position), and this is what
    // gets added to basepos in addition to the inter-section increment
    // to compute the first position of the next section.
    Xapian::termpos curpos;
    Xapian::WritableDatabase& wdb;

    TextSplitDb(Xapian::WritableDatabase& _wdb, Xapian::Document &d,
                TermProc *prc)
        : TextSplitP(prc), doc(d), basepos(1), curpos(0), wdb(_wdb)
        {}

    // Reimplement text_to_words to insert the begin and end anchor terms.
    virtual bool text_to_words(const string &in) {
        string ermsg;

        try {
            // Index the possibly prefixed start term.
            doc.add_posting(ft.pfx + start_of_field_term, basepos, ft.wdfinc);
            ++basepos;
        } XCATCHERROR(ermsg);
        if (!ermsg.empty()) {
            LOGERR("Db: xapian add_posting error " << ermsg << "\n");
            goto out;
        }

        if (!TextSplitP::text_to_words(in)) {
            LOGDEB("TextSplitDb: TextSplit::text_to_words failed\n");
            goto out;
        }

        try {
            // Index the possibly prefixed end term.
            doc.add_posting(ft.pfx + end_of_field_term, basepos + curpos + 1,
                            ft.wdfinc);
            ++basepos;
        } XCATCHERROR(ermsg);
        if (!ermsg.empty()) {
            LOGERR("Db: xapian add_posting error " << ermsg << "\n");
            goto out;
        }

    out:
        basepos += curpos + 100;
        return true;
    }

    void setTraits(const FieldTraits& ftp) {
        ft = ftp;
        if (!ft.pfx.empty())
            ft.pfx = wrap_prefix(ft.pfx);
    }

    friend class TermProcIdx;

private:
    FieldTraits ft;
};

class TermProcIdx : public TermProc {
public:
    TermProcIdx() : TermProc(0), m_ts(0), m_lastpagepos(0), m_pageincr(0) {}
    void setTSD(TextSplitDb *ts) {m_ts = ts;}

    bool takeword(const std::string &term, int pos, int, int) {
        // Compute absolute position (pos is relative to current segment),
        // and remember relative.
        m_ts->curpos = pos;
        pos += m_ts->basepos;
        // Don't try to add empty term Xapian doesnt like it... Safety check
        // this should not happen.
        if (term.empty())
            return true;
        string ermsg;
        try {
            // Index without prefix, using the field-specific weighting
            LOGDEB1("Emitting term at " << pos << " : [" << term << "]\n");
            if (!m_ts->ft.pfxonly)
                m_ts->doc.add_posting(term, pos, m_ts->ft.wdfinc);

#ifdef TESTING_XAPIAN_SPELL
            if (Db::isSpellingCandidate(term, false)) {
                m_ts->wdb.add_spelling(term);
            }
#endif
            // Index the prefixed term.
            if (!m_ts->ft.pfx.empty()) {
                m_ts->doc.add_posting(m_ts->ft.pfx + term, pos, 
                                      m_ts->ft.wdfinc);
            }
            return true;
        } XCATCHERROR(ermsg);
        LOGERR("Db: xapian add_posting error " << ermsg << "\n");
        return false;
    }

    void newpage(int pos)  {
        pos += m_ts->basepos;
        if (pos < int(baseTextPosition)) {
            LOGDEB("newpage: not in body: " << pos << "\n");
            return;
        }

        m_ts->doc.add_posting(m_ts->ft.pfx + page_break_term, pos);
        if (pos == m_lastpagepos) {
            m_pageincr++;
            LOGDEB2("newpage: same pos, pageincr " << m_pageincr <<
                    " lastpagepos " << m_lastpagepos << "\n");
        } else {
            LOGDEB2("newpage: pos change, pageincr " << m_pageincr <<
                    " lastpagepos " << m_lastpagepos << "\n");
            if (m_pageincr > 0) {
                // Remember the multiple page break at this position
                unsigned int relpos = m_lastpagepos - baseTextPosition;
                LOGDEB2("Remembering multiple page break. Relpos " << relpos <<
                        " cnt " << m_pageincr << "\n");
                m_pageincrvec.push_back(pair<int, int>(relpos, m_pageincr));
            }
            m_pageincr = 0;
        }
        m_lastpagepos = pos;
    }

    virtual bool flush() {
        if (m_pageincr > 0) {
            unsigned int relpos = m_lastpagepos - baseTextPosition;
            LOGDEB2("Remembering multiple page break. Position " << relpos <<
                    " cnt " << m_pageincr << "\n");
            m_pageincrvec.push_back(pair<int, int>(relpos, m_pageincr));
            m_pageincr = 0;
        }
        return TermProc::flush();
    }

    TextSplitDb *m_ts;
    // Auxiliary page breaks data for positions with multiple page breaks.
    int m_lastpagepos;
    // increment of page breaks at same pos. Normally 0, 1.. when several
    // breaks at the same pos
    int m_pageincr; 
    vector <pair<int, int> > m_pageincrvec;
};

DbW::NativeW::NativeW(Db *db) 
    : Db::Native(db)
#ifdef IDX_THREADS
    , m_wqueue("DbUpd", m_rcldb->m_config->getThrConf(RclConfig::ThrDbWrite).first),
      m_totalworkns(0LL), m_havewriteq(false)
#endif // IDX_THREADS
{ 
    m_iswritable = true;
    m_noversionwrite = false;
    m_rcldbw = dynamic_cast<DbW*>(db);
    LOGDEB1("Native::Native: me " << this << "\n");
}

DbW::NativeW::~NativeW() 
{ 
    LOGDEB1("NativeW::~NativeW: me " << this << "\n");
#ifdef IDX_THREADS
    if (m_havewriteq) {
        void *status = m_wqueue.setTerminateAndWait();
        if (status) {
            LOGDEB1("NativeW::~NativeW: worker status " << status << "\n");
        }
    }
#endif // IDX_THREADS
}


#ifdef IDX_THREADS
void *DbUpdWorker(void* vdbp)
{
    recoll_threadinit();
    DbW::NativeW *ndbp = (DbW::NativeW *)vdbp;
    WorkQueue<DbUpdTask*> *tqp = &(ndbp->m_wqueue);

    DbUpdTask *tsk = 0;
    for (;;) {
        size_t qsz = -1;
        if (!tqp->take(&tsk, &qsz)) {
            tqp->workerExit();
            return (void*)1;
        }
        bool status = false;
        switch (tsk->op) {
        case DbUpdTask::AddOrUpdate:
            LOGDEB("DbUpdWorker: got add/update task, ql " << qsz << "\n");
            status = ndbp->addOrUpdateWrite(
                tsk->udi, tsk->uniterm, tsk->doc, tsk->txtlen, tsk->rawztext);
            break;
        case DbUpdTask::Delete:
            LOGDEB("DbUpdWorker: got delete task, ql " << qsz << "\n");
            status = ndbp->purgeFileWrite(false, tsk->udi, tsk->uniterm);
            break;
        case DbUpdTask::PurgeOrphans:
            LOGDEB("DbUpdWorker: got orphans purge task, ql " << qsz << "\n");
            status = ndbp->purgeFileWrite(true, tsk->udi, tsk->uniterm);
            break;
        default:
            LOGERR("DbUpdWorker: unknown op " << tsk->op << " !!\n");
            break;
        }
        if (!status) {
            LOGERR("DbUpdWorker: xxWrite failed\n");
            tqp->workerExit();
            delete tsk;
            return (void*)0;
        }
        delete tsk;
    }
}

void DbW::NativeW::maybeStartThreads()
{
    m_havewriteq = false;
    const RclConfig *cnf = m_rcldb->m_config;
    int writeqlen = cnf->getThrConf(RclConfig::ThrDbWrite).first;
    int writethreads = cnf->getThrConf(RclConfig::ThrDbWrite).second;
    if (writethreads > 1) {
        LOGINFO("RclDb: write threads count was forced down to 1\n");
        writethreads = 1;
    }
    if (writeqlen >= 0 && writethreads > 0) {
        if (!m_wqueue.start(writethreads, DbUpdWorker, this)) {
            LOGERR("Db::Db: Worker start failed\n");
            return;
        }
        m_havewriteq = true;
    }
    LOGDEB("RclDb:: threads: haveWriteQ " << m_havewriteq << ", wqlen " <<
           writeqlen << " wqts " << writethreads << "\n");
}

#endif // IDX_THREADS

void DbW::NativeW::openWrite(const string& dir, Db::OpenMode mode)
{
    int action = (mode == Db::DbUpd) ? Xapian::DB_CREATE_OR_OPEN :
        Xapian::DB_CREATE_OR_OVERWRITE;
    updated = vector<bool>(xwdb.get_lastdocid() + 1, false);

#ifdef _WIN32
    // On Windows, Xapian is quite bad at erasing partial db which can
    // occur because of open file deletion errors.
    if (mode == DbTrunc) {
        if (path_exists(path_cat(dir, "iamchert"))) {
            wipedir(dir);
            path_unlink(dir);
        }
    }
#endif
    
    if (path_exists(dir)) {
        // Existing index. 
        xwdb = Xapian::WritableDatabase(dir, action);
        if (action == Xapian::DB_CREATE_OR_OVERWRITE ||
            xwdb.get_doccount() == 0) {
            // New or empty index. Set the "store text" option
            // according to configuration. The metadata record will be
            // written further down.
            m_storetext = o_index_storedoctext;
            LOGDEB("Db:: index " << (m_storetext?"stores":"does not store") <<
                   " document text\n");
        } else {
            // Existing non empty. Get the option from the index.
            storesDocText(xwdb);
        }
    } else {
        // New index. If possible, and depending on config, use a stub
        // to force using Chert. No sense in doing this if we are
        // storing the text anyway.
#if XAPIAN_AT_LEAST(1,3,0) && XAPIAN_HAS_CHERT_BACKEND
        // Xapian with Glass and Chert support. If storedoctext is
        // specified in the configuration, use the default backend
        // (Glass), else force Chert. There might be reasons why
        // someone would want to use Chert and store text anyway, but
        // it's an exotic case, and things are complicated enough
        // already.
        if (o_index_storedoctext) {
            xwdb = Xapian::WritableDatabase(dir, action);
            m_storetext = true;
        } else {
            // Force Chert format, don't store the text.
            string stub = path_cat(m_rcldb->m_config->getConfDir(),
                                   "xapian.stub");
            std::fstream fp;
            if (!path_streamopen(stub, std::ios::out|std::ios::trunc, fp)) {
                throw(string("Can't create ") + stub);
            }
            fp << "chert " << dir << "\n";
            fp.close();
            xwdb = Xapian::WritableDatabase(stub, action);
            m_storetext = false;
        }
        LOGINF("Rcl::Db::openWrite: new index will " << (m_storetext?"":"not ")
               << "store document text\n");
#else
        // Old Xapian (chert only) or much newer (no chert). Use the
        // default index backend and let the user decide of the
        // abstract generation method. The configured default is to
        // store the text.
        xwdb = Xapian::WritableDatabase(dir, action);
        m_storetext = o_index_storedoctext;
#endif
    }

    // If the index is empty, write the data format version, 
    // and the storetext option value inside the index descriptor (new
    // with recoll 1.24, maybe we'll have other stuff to store in
    // there in the future).
    if (xwdb.get_doccount() == 0) {
        string desc = string("storetext=") + (m_storetext ? "1" : "0") + "\n";
        xwdb.set_metadata(cstr_RCL_IDX_DESCRIPTOR_KEY, desc);
        xwdb.set_metadata(cstr_RCL_IDX_VERSION_KEY, cstr_RCL_IDX_VERSION);
    }
    LOGDEB("Db::open: lastdocid: " << xwdb.get_lastdocid() <<"\n");

    m_iswritable = true;
    // We used to open a readonly object in addition to the
    // r/w one because some operations were faster when
    // performed through a Database: no forced flushes on
    // allterms_begin(), used in subDocs(). This issue has
    // been gone for a long time (now: Xapian 1.2) and the
    // separate objects seem to trigger other Xapian issues,
    // so the query db is now a clone of the update one.
    xrdb = xwdb;

#ifdef IDX_THREADS
    maybeStartThreads();
#endif
}

void DbW::NativeW::closeWrite()
{
#ifdef IDX_THREADS
    m_rcldbw->waitUpdIdle();
#endif
    if (!m_noversionwrite)
        xwdb.set_metadata(cstr_RCL_IDX_VERSION_KEY, cstr_RCL_IDX_VERSION);
    LOGDEB("Rcl::Db:close: xapian will close. May take some time\n");
}


// Clear term from document if its frequency is 0. This should
// probably be done by Xapian when the freq goes to 0 when removing a
// posting, but we have to do it ourselves
bool DbW::NativeW::clearDocTermIfWdf0(Xapian::Document& xdoc, const string& term)
{
    LOGDEB1("Db::clearDocTermIfWdf0: [" << term << "]\n");

    // Find the term
    Xapian::TermIterator xit;
    XAPTRY(xit = xdoc.termlist_begin(); xit.skip_to(term);,
           xrdb, m_rcldb->m_reason);
    if (!m_rcldb->m_reason.empty()) {
        LOGERR("Db::clearDocTerm...: [" << term << "] skip failed: " <<
               m_rcldb->m_reason << "\n");
        return false;
    }
    if (xit == xdoc.termlist_end() || term.compare(*xit)) {
        LOGDEB0("Db::clearDocTermIFWdf0: term [" << term <<
                "] not found. xit: [" <<
                (xit == xdoc.termlist_end() ? "EOL": *xit) << "]\n");
        return false;
    }

    // Clear the term if its frequency is 0
    if (xit.get_wdf() == 0) {
        LOGDEB1("Db::clearDocTermIfWdf0: clearing [" << term << "]\n");
        XAPTRY(xdoc.remove_term(term), xwdb, m_rcldb->m_reason);
        if (!m_rcldb->m_reason.empty()) {
            LOGDEB0("Db::clearDocTermIfWdf0: failed [" << term << "]: " <<
                    m_rcldb->m_reason << "\n");
        }
    }
    return true;
}



// Holder for term + pos
struct DocPosting {
    DocPosting(string t, Xapian::termpos ps)
        : term(t), pos(ps) {}
    string term;
    Xapian::termpos pos;
};

// Clear all terms for given field for given document.
// The terms to be cleared are all those with the appropriate
// prefix. We also remove the postings for the unprefixed terms (that
// is, we undo what we did when indexing).
bool DbW::NativeW::clearField(Xapian::Document& xdoc, const string& pfx,
                              Xapian::termcount wdfdec)
{
    LOGDEB1("Db::clearField: clearing prefix [" << pfx << "] for docid " <<
            xdoc.get_docid() << "\n");

    vector<DocPosting> eraselist;

    string wrapd = wrap_prefix(pfx);

    m_rcldb->m_reason.clear();
    for (int tries = 0; tries < 2; tries++) {
        try {
            Xapian::TermIterator xit;
            xit = xdoc.termlist_begin();
            xit.skip_to(wrapd);
            while (xit != xdoc.termlist_end() && 
                   !(*xit).compare(0, wrapd.size(), wrapd)) {
                LOGDEB1("Db::clearfield: erasing for [" << *xit << "]\n");
                Xapian::PositionIterator posit;
                for (posit = xit.positionlist_begin();
                     posit != xit.positionlist_end(); posit++) {
                    eraselist.push_back(DocPosting(*xit, *posit));
                    eraselist.push_back(DocPosting(strip_prefix(*xit), *posit));
                }
                xit++;
            }
        } catch (const Xapian::DatabaseModifiedError &e) {
            m_rcldb->m_reason = e.get_msg();
            xrdb.reopen();
            continue;
        } XCATCHERROR(m_rcldb->m_reason);
        break;
    }
    if (!m_rcldb->m_reason.empty()) {
        LOGERR("Db::clearField: failed building erase list: " <<
               m_rcldb->m_reason << "\n");
        return false;
    }

    // Now remove the found positions, and the terms if the wdf is 0
    for (vector<DocPosting>::const_iterator it = eraselist.begin();
         it != eraselist.end(); it++) {
        LOGDEB1("Db::clearField: remove posting: [" << it->term << "] pos [" <<
                it->pos << "]\n");
        XAPTRY(xdoc.remove_posting(it->term, it->pos, wdfdec);, 
               xwdb,m_rcldb->m_reason);
        if (!m_rcldb->m_reason.empty()) {
            // Not that this normally fails for non-prefixed XXST and
            // ND, don't make a fuss
            LOGDEB1("Db::clearFiedl: remove_posting failed for [" << it->term <<
                    "]," << it->pos << ": " << m_rcldb->m_reason << "\n");
        }
        clearDocTermIfWdf0(xdoc, it->term);
    }
    return true;
}

// Note: we're passed a Xapian::Document* because Xapian
// reference-counting is not mt-safe. We take ownership and need
// to delete it before returning.
bool DbW::NativeW::addOrUpdateWrite(
    const string& udi, const string& uniterm, Xapian::Document *newdocument_ptr, 
    size_t textlen, const string& rawztext)
{
#ifdef IDX_THREADS
    Chrono chron;
    std::unique_lock<std::mutex> lock(m_mutex);
#endif
    std::unique_ptr<Xapian::Document> doc_cleaner(newdocument_ptr);

    // Check file system full every mbyte of indexed text. It's a bit wasteful
    // to do this after having prepared the document, but it needs to be in
    // the single-threaded section.
    if (m_rcldb->m_maxFsOccupPc > 0 && 
        (m_rcldbw->m_occFirstCheck || 
         (m_rcldbw->m_curtxtsz - m_rcldbw->m_occtxtsz) / MB >= 1)) {
        LOGDEB("Db::add: checking file system usage\n");
        int pc;
        m_rcldbw->m_occFirstCheck = 0;
        if (fsocc(m_rcldb->m_basedir, &pc) && pc >= m_rcldb->m_maxFsOccupPc) {
            LOGERR("Db::add: stop indexing: file system " << pc << " %" <<
                   " full > max " << m_rcldb->m_maxFsOccupPc << " %" << "\n");
            return false;
        }
        m_rcldbw->m_occtxtsz = m_rcldbw->m_curtxtsz;
    }

    const char *fnc = udi.c_str();
    string ermsg;

    // Add db entry or update existing entry:
    Xapian::docid did = 0;
    try {
        did = xwdb.replace_document(uniterm, *newdocument_ptr);
        if (did < updated.size()) {
            // This is necessary because only the file-level docs are tested
            // by needUpdate(), so the subdocs existence flags are only set
            // here.
            updated[did] = true;
            LOGINFO("Db::add: docid " << did << " updated [" << fnc << "]\n");
        } else {
            LOGINFO("Db::add: docid " << did << " added [" << fnc << "]\n");
        }
    } XCATCHERROR(ermsg);
    if (!ermsg.empty()) {
        LOGERR("Db::add: replace_document failed: " << ermsg << "\n");
        ermsg.erase();
        // FIXME: is this ever actually needed?
        try {
            xwdb.add_document(*newdocument_ptr);
            LOGDEB("Db::add: " << fnc <<
                   " added (failed re-seek for duplicate)\n");
        } XCATCHERROR(ermsg);
        if (!ermsg.empty()) {
            LOGERR("Db::add: add_document failed: " << ermsg << "\n");
            return false;
        }
    }

    XAPTRY(xwdb.set_metadata(rawtextMetaKey(did), rawztext),
           xwdb, m_rcldb->m_reason);
    if (!m_rcldb->m_reason.empty()) {
        LOGERR("Db::addOrUpdate: set_metadata error: " <<
               m_rcldb->m_reason << "\n");
        // This only affects snippets, so let's say not fatal
    }
    
    // Test if we're over the flush threshold (limit memory usage):
    bool ret = m_rcldbw->maybeflush(textlen);
#ifdef IDX_THREADS
    m_totalworkns += chron.nanos();
#endif
    return ret;
}

bool DbW::NativeW::purgeFileWrite(bool orphansOnly, const string& udi, const string& uniterm)
{
#if defined(IDX_THREADS) 
    // We need a mutex even if we have a write queue (so we can only
    // be called by a single thread) to protect about multiple acces
    // to xrdb from subDocs() which is also called from needupdate()
    // (called from outside the write thread !
    std::unique_lock<std::mutex> lock(m_mutex);
#endif // IDX_THREADS

    string ermsg;
    try {
        Xapian::PostingIterator docid = xwdb.postlist_begin(uniterm);
        if (docid == xwdb.postlist_end(uniterm)) {
            return true;
        }
        if (m_rcldbw->m_flushMb > 0) {
            Xapian::termcount trms = xwdb.get_doclength(*docid);
            m_rcldbw->maybeflush(trms * 5);
        }
        string sig;
        if (orphansOnly) {
            Xapian::Document doc = xwdb.get_document(*docid);
            sig = doc.get_value(VALUE_SIG);
            if (sig.empty()) {
                LOGINFO("purgeFileWrite: got empty sig\n");
                return false;
            }
        } else {
            LOGDEB("purgeFile: delete docid " << *docid << "\n");
            deleteDocument(*docid);
        }
        vector<Xapian::docid> docids;
        subDocs(udi, 0, docids);
        LOGDEB("purgeFile: subdocs cnt " << docids.size() << "\n");
        for (vector<Xapian::docid>::iterator it = docids.begin();
             it != docids.end(); it++) {
            if (m_rcldbw->m_flushMb > 0) {
                Xapian::termcount trms = xwdb.get_doclength(*it);
                m_rcldbw->maybeflush(trms * 5);
            }
            string subdocsig;
            if (orphansOnly) {
                Xapian::Document doc = xwdb.get_document(*it);
                subdocsig = doc.get_value(VALUE_SIG);
                if (subdocsig.empty()) {
                    LOGINFO("purgeFileWrite: got empty sig for subdoc??\n");
                    continue;
                }
            }
        
            if (!orphansOnly || sig != subdocsig) {
                LOGDEB("Db::purgeFile: delete subdoc " << *it << "\n");
                deleteDocument(*it);
            }
        }
        return true;
    } XCATCHERROR(ermsg);
    if (!ermsg.empty()) {
        LOGERR("Db::purgeFileWrite: " << ermsg << "\n");
    }
    return false;
}

#define RECORD_APPEND(R, NM, VAL) {R += NM + "=" + VAL + "\n";}

bool DbW::NativeW::docToXdocXattrOnly(TextSplitDb *splitter, const string &udi, 
                                      Doc &doc, Xapian::Document& xdoc)
{
    LOGDEB0("Db::docToXdocXattrOnly\n");
#ifdef IDX_THREADS
    std::unique_lock<std::mutex> lock(m_mutex);
#endif

    // Read existing document and its data record
    if (getDoc(udi, 0, xdoc) == 0) {
        LOGERR("docToXdocXattrOnly: existing doc not found\n");
        return false;
    }
    string data;
    XAPTRY(data = xdoc.get_data(), xrdb, m_rcldb->m_reason);
    if (!m_rcldb->m_reason.empty()) {
        LOGERR("Db::xattrOnly: got error: " << m_rcldb->m_reason << "\n");
        return false;
    }

    // Clear the term lists for the incoming fields and index the new values
    map<string, string>::iterator meta_it;
    for (const auto& ent : doc.meta) {
        const FieldTraits *ftp;
        if (!m_rcldb->fieldToTraits(ent.first, &ftp) || ftp->pfx.empty()) {
            LOGDEB0("Db::xattrOnly: no prefix for field [" <<
                    ent.first << "], skipped\n");
            continue;
        }
        // Clear the previous terms for the field
        clearField(xdoc, ftp->pfx, ftp->wdfinc);
        LOGDEB0("Db::xattrOnly: field [" << ent.first << "] pfx [" <<
                ftp->pfx << "] inc " << ftp->wdfinc << ": [" <<
                ent.second << "]\n");
        splitter->setTraits(*ftp);
        if (!splitter->text_to_words(ent.second)) {
            LOGDEB("Db::xattrOnly: split failed for " << ent.first << "\n");
        }
    }
    xdoc.add_value(VALUE_SIG, doc.sig);

    // Parse current data record into a dict for ease of processing
    ConfSimple datadic(data);
    if (!datadic.ok()) {
        LOGERR("db::docToXdocXattrOnly: failed turning data rec to dict\n");
        return false;
    }

    // For each "stored" field, check if set in doc metadata and
    // update the value if it is
    const set<string>& stored = m_rcldb->m_config->getStoredFields();
    for (set<string>::const_iterator it = stored.begin();
         it != stored.end(); it++) {
        string nm = m_rcldb->m_config->fieldCanon(*it);
        if (doc.getmeta(nm, 0)) {
            string value = neutchars(
                truncate_to_word(doc.meta[nm], m_rcldb->m_idxMetaStoredLen), 
                cstr_nc);
            datadic.set(nm, value, "");
        }
    }

    // Recreate the record. We want to do this with the local RECORD_APPEND
    // method for consistency in format, instead of using ConfSimple print
    vector<string> names = datadic.getNames("");
    data.clear();
    for (vector<string>::const_iterator it = names.begin(); 
         it != names.end(); it++) {
        string value;
        datadic.get(*it, value, "");
        RECORD_APPEND(data, *it, value);
    }
    RECORD_APPEND(data, Doc::keysig, doc.sig);
    xdoc.set_data(data);
    return true;
}

/////////////////////////////////////////////////////////////////////////////////////////

DbW::DbW(const RclConfig *cfp)
    : Db(cfp)
{
    m_ndbw = new NativeW(this);
    delete m_ndb;
    m_ndb = m_ndbw;
}

DbW::~DbW()
{
}

// Add document in internal form to the database: index the terms in
// the title abstract and body and add special terms for file name,
// date, mime type etc. , create the document data record (more
// metadata), and update database
bool DbW::addOrUpdate(const string &udi, const string &parent_udi, Doc &doc)
{
    LOGDEB("Db::add: udi [" << udi << "] parent [" << parent_udi << "]\n");
    if (m_ndbw == 0)
        return false;

    // This document is potentially going to be passed to the index
    // update thread. The reference counters are not mt-safe, so we
    // need to do this through a pointer. The reference is just there
    // to avoid changing too much code (the previous version passed a copy).
    Xapian::Document *newdocument_ptr = new Xapian::Document;
    Xapian::Document &newdocument(*newdocument_ptr);
    
    // The term processing pipeline:
    TermProcIdx tpidx;
    TermProc *nxt = &tpidx;
    TermProcStop tpstop(nxt, m_stops);nxt = &tpstop;
    //TermProcCommongrams tpcommon(nxt, m_stops); nxt = &tpcommon;

    TermProcMulti tpmulti(nxt, m_syngroups);
    if (m_syngroups.getmultiwordsmaxlength() > 1) {
        nxt = &tpmulti;
    }

    TermProcPrep tpprep(nxt);
    if (o_index_stripchars)
        nxt = &tpprep;
    
    TextSplitDb splitter(m_ndbw->xwdb, newdocument, nxt);
    tpidx.setTSD(&splitter);

    // Udi unique term: this is used for file existence/uptodate
    // checks, and unique id for the replace_document() call.
    string uniterm = make_uniterm(udi);
    string rawztext; // Doc compressed text

    if (doc.onlyxattr) {
        // Only updating an existing doc with new extended attributes
        // data.  Need to read the old doc and its data record
        // first. This is so different from the normal processing that
        // it uses a fully separate code path (with some duplication
        // unfortunately)
        if (!m_ndbw->docToXdocXattrOnly(&splitter, udi, doc, newdocument)) {
            delete newdocument_ptr;
            return false;
        }
    } else {

        if (m_idxTextTruncateLen > 0) {
            doc.text = truncate_to_word(doc.text, m_idxTextTruncateLen);
        }
        
        // If the ipath is like a path, index the last element. This is
        // for compound documents like zip and chm for which the filter
        // uses the file path as ipath. 
        if (!doc.ipath.empty() && 
            doc.ipath.find_first_not_of("0123456789") != string::npos) {
            string utf8ipathlast;
            // There is no way in hell we could have an idea of the
            // charset here, so let's hope it's ascii or utf-8. We call
            // transcode to strip the bad chars and pray
            if (transcode(path_getsimple(doc.ipath), utf8ipathlast, "UTF-8", "UTF-8")) {
                splitter.text_to_words(utf8ipathlast);
            }
        }

        // Split and index the path from the url for path-based filtering
        {
            string path = url_gpathS(doc.url);

#ifdef _WIN32
            // Windows file names are case-insensitive, so we
            // translate to UTF-8 and lowercase
            string upath = compute_utf8fn(m_config, path, false);            
            unacmaybefold(upath, path, "UTF-8", UNACOP_FOLD);
#endif

            vector<string> vpath;
            stringToTokens(path, vpath, "/");
            // If vpath is not /, the last elt is the file/dir name, not a
            // part of the path.
            if (vpath.size())
                vpath.resize(vpath.size()-1);
            splitter.curpos = 0;
            newdocument.add_posting(wrap_prefix(pathelt_prefix),
                                    splitter.basepos + splitter.curpos++);
            for (auto& elt : vpath) {
                if (elt.length() > 230) {
                    // Just truncate it. May still be useful because of wildcards
                    elt = elt.substr(0, 230);
                }
                newdocument.add_posting(wrap_prefix(pathelt_prefix) + elt, 
                                        splitter.basepos + splitter.curpos++);
            }
            splitter.basepos += splitter.curpos + 100;
        }

        // Index textual metadata.  These are all indexed as text with
        // positions, as we may want to do phrase searches with them (this
        // makes no sense for keywords by the way).
        //
        // The order has no importance, and we set a position gap of 100
        // between fields to avoid false proximity matches.
        for (const auto& entry: doc.meta) {
            if (entry.second.empty()) {
                continue;
            }
            const FieldTraits *ftp{nullptr};
            fieldToTraits(entry.first, &ftp);
            if (ftp && ftp->valueslot) {
                LOGDEB("Adding value: for field " << entry.first << " slot "
                       << ftp->valueslot << endl);
                add_field_value(newdocument, *ftp, entry.second);
            }

            // There was an old comment here about not testing for
            // empty prefix, and we indeed did not test. I don't think
            // that it makes sense any more (and was in disagreement
            // with the LOG message. Really now: no prefix: no
            // indexing.
            if (ftp && !ftp->pfx.empty()) {
                LOGDEB0("Db::add: field [" << entry.first << "] pfx [" <<
                        ftp->pfx << "] inc " << ftp->wdfinc << ": [" <<
                        entry.second << "]\n");
                splitter.setTraits(*ftp);
                if (!splitter.text_to_words(entry.second)) {
                    LOGDEB("Db::addOrUpdate: split failed for " <<
                           entry.first << "\n");
                }
            } else {
                LOGDEB0("Db::add: no prefix for field [" <<
                        entry.first << "], no indexing\n");
            }
        }

        // Reset to no prefix and default params
        splitter.setTraits(FieldTraits());

        if (splitter.curpos < baseTextPosition)
            splitter.basepos = baseTextPosition;

        // Split and index body text
        LOGDEB2("Db::add: split body: [" << doc.text << "]\n");

#ifdef TEXTSPLIT_STATS
        splitter.resetStats();
#endif
        if (!splitter.text_to_words(doc.text)) {
            LOGDEB("Db::addOrUpdate: split failed for main text\n");
        } else {
            if (m_ndbw->m_storetext) {
                ZLibUtBuf buf;
                deflateToBuf(doc.text.c_str(), doc.text.size(), buf);
                rawztext.assign(buf.getBuf(), buf.getCnt());
            }
        }

#ifdef TEXTSPLIT_STATS
        // Reject bad data. unrecognized base64 text is characterized by
        // high avg word length and high variation (because there are
        // word-splitters like +/ inside the data).
        TextSplit::Stats::Values v = splitter.getStats();
        // v.avglen > 15 && v.sigma > 12 
        if (v.count > 200 && (v.avglen > 10 && v.sigma / v.avglen > 0.8)) {
            LOGINFO("RclDb::addOrUpdate: rejecting doc for bad stats count " <<
                    v.count << " avglen " << v.avglen << " sigma " << v.sigma <<
                    " url [" << doc.url << "] ipath [" << doc.ipath <<
                    "] text " << doc.text << "\n");
            delete newdocument_ptr;
            return true;
        }
#endif

        ////// Special terms for other metadata. No positions for these.
        // Mime type
        newdocument.add_boolean_term(wrap_prefix(mimetype_prefix) + doc.mimetype);

        // Simple file name indexed unsplit for specific "file name"
        // searches. This is not the same as a filename: clause inside the
        // query language.
        // We also add a term for the filename extension if any.
        string utf8fn;
        if (doc.getmeta(Doc::keyfn, &utf8fn) && !utf8fn.empty()) {
            string fn;
            if (unacmaybefold(utf8fn, fn, "UTF-8", UNACOP_UNACFOLD)) {
                // We should truncate after extracting the extension,
                // but this is a pathological case anyway
                if (fn.size() > 230)
                    utf8truncate(fn, 230);
                string::size_type pos = fn.rfind('.');
                if (pos != string::npos && pos != fn.length() - 1) {
                    newdocument.add_boolean_term(wrap_prefix(fileext_prefix) + 
                                                 fn.substr(pos + 1));
                }
                newdocument.add_term(wrap_prefix(unsplitfilename_prefix) + fn,0);
            }
        }

        newdocument.add_boolean_term(uniterm);
        // Parent term. This is used to find all descendents, mostly
        // to delete them when the parent goes away
        if (!parent_udi.empty()) {
            newdocument.add_boolean_term(make_parentterm(parent_udi));
        }

        // Fields used for selecting by date. Note that this only
        // works for years AD 0-9999 (no crash elsewhere, but things
        // won't work).
        time_t mtime = atoll(doc.dmtime.empty() ? doc.fmtime.c_str() : 
                             doc.dmtime.c_str());
        struct tm tmb;
        localtime_r(&mtime, &tmb);
        char buf[50]; // It's actually 9, but use 50 to suppress warnings.
        snprintf(buf, 50, "%04d%02d%02d",
                 tmb.tm_year+1900, tmb.tm_mon + 1, tmb.tm_mday);
            
        // Date (YYYYMMDD)
        newdocument.add_boolean_term(wrap_prefix(xapday_prefix) + string(buf)); 
        // Month (YYYYMM)
        buf[6] = '\0';
        newdocument.add_boolean_term(wrap_prefix(xapmonth_prefix) + string(buf));
        // Year (YYYY)
        buf[4] = '\0';
        newdocument.add_boolean_term(wrap_prefix(xapyear_prefix) + string(buf)); 


        //////////////////////////////////////////////////////////////////
        // Document data record. omindex has the following nl separated fields:
        // - url
        // - sample
        // - caption (title limited to 100 chars)
        // - mime type 
        //
        // The title, author, abstract and keywords fields are special,
        // they always get stored in the document data
        // record. Configurable other fields can be, too.
        //
        // We truncate stored fields abstract, title and keywords to
        // reasonable lengths and suppress newlines (so that the data
        // record can keep a simple syntax)

        string record;
        RECORD_APPEND(record, Doc::keyurl, doc.url);
        RECORD_APPEND(record, Doc::keytp, doc.mimetype);
        // We left-zero-pad the times so that they are lexico-sortable
        leftzeropad(doc.fmtime, 11);
        RECORD_APPEND(record, Doc::keyfmt, doc.fmtime);
        if (!doc.dmtime.empty()) {
            leftzeropad(doc.dmtime, 11);
            RECORD_APPEND(record, Doc::keydmt, doc.dmtime);
        }
        RECORD_APPEND(record, Doc::keyoc, doc.origcharset);

        if (doc.fbytes.empty())
            doc.fbytes = doc.pcbytes;

        if (!doc.fbytes.empty()) {
            RECORD_APPEND(record, Doc::keyfs, doc.fbytes);
            leftzeropad(doc.fbytes, 12);
            newdocument.add_value(VALUE_SIZE, doc.fbytes);
        }
        if (doc.haschildren) {
            newdocument.add_boolean_term(has_children_term);
        }   
        if (!doc.pcbytes.empty())
            RECORD_APPEND(record, Doc::keypcs, doc.pcbytes);
        char sizebuf[30]; 
        sprintf(sizebuf, "%u", (unsigned int)doc.text.length());
        RECORD_APPEND(record, Doc::keyds, sizebuf);

        // Note that we add the signature both as a value and in the data record
        if (!doc.sig.empty()) {
            RECORD_APPEND(record, Doc::keysig, doc.sig);
            newdocument.add_value(VALUE_SIG, doc.sig);
        }

        if (!doc.ipath.empty())
            RECORD_APPEND(record, Doc::keyipt, doc.ipath);

        // Fields from the Meta array. Handle title specially because it has a 
        // different name inside the data record (history...)
        string& ttref = doc.meta[Doc::keytt];
        ttref = neutchars(truncate_to_word(ttref, m_idxMetaStoredLen), cstr_nc);
        if (!ttref.empty()) {
            RECORD_APPEND(record, cstr_caption, ttref);
            ttref.clear();
        }

        // If abstract is empty, we make up one with the beginning of the
        // document. This is then not indexed, but part of the doc data so
        // that we can return it to a query without having to decode the
        // original file.
        // Note that the map accesses by operator[] create empty entries if they
        // don't exist yet.
        if (m_idxAbsTruncLen > 0) {
            string& absref = doc.meta[Doc::keyabs];
            trimstring(absref, " \t\r\n");
            if (absref.empty()) {
                if (!doc.text.empty())
                    absref = cstr_syntAbs + 
                        neutchars(truncate_to_word(doc.text, m_idxAbsTruncLen), 
                                  cstr_nc);
            } else {
                absref = neutchars(truncate_to_word(absref, m_idxAbsTruncLen), 
                                   cstr_nc);
            }
            // Do the append here to avoid the different truncation done
            // in the regular "stored" loop
            if (!absref.empty()) {
                RECORD_APPEND(record, Doc::keyabs, absref);
                absref.clear();
            }
        }
        
        // Append all regular "stored" meta fields
        const set<string>& stored = m_config->getStoredFields();
        for (set<string>::const_iterator it = stored.begin();
             it != stored.end(); it++) {
            string nm = m_config->fieldCanon(*it);
            if (!doc.meta[nm].empty()) {
                string value = 
                    neutchars(truncate_to_word(doc.meta[nm], 
                                               m_idxMetaStoredLen), cstr_nc);
                RECORD_APPEND(record, nm, value);
            }
        }

        // At this point, if the document "filename" field was empty,
        // try to store the "container file name" value. This is done
        // after indexing because we don't want search matches on
        // this, but the filename is often useful for display
        // purposes.
        const string *fnp = 0;
        if (!doc.peekmeta(Rcl::Doc::keyfn, &fnp) || fnp->empty()) {
            if (doc.peekmeta(Rcl::Doc::keyctfn, &fnp) && !fnp->empty()) {
                string value = neutchars(truncate_to_word(*fnp, m_idxMetaStoredLen), cstr_nc);
                RECORD_APPEND(record, Rcl::Doc::keyfn, value);
            }
        }

        // If empty pages (multiple break at same pos) were recorded, save
        // them (this is because we have no way to record them in the
        // Xapian list
        if (!tpidx.m_pageincrvec.empty()) {
            ostringstream multibreaks;
            for (unsigned int i = 0; i < tpidx.m_pageincrvec.size(); i++) {
                if (i != 0)
                    multibreaks << ",";
                multibreaks << tpidx.m_pageincrvec[i].first << "," << 
                    tpidx.m_pageincrvec[i].second;
            }
            RECORD_APPEND(record, string(cstr_mbreaks), multibreaks.str());
        }
    
        // If the file's md5 was computed, add value and term.  The
        // value is optionally used for query result duplicate
        // elimination, and the term to find the duplicates (XM is the
        // prefix for rclmd5 in fields) We don't do this for empty
        // docs.
        const string *md5;
        if (doc.peekmeta(Doc::keymd5, &md5) && !md5->empty() &&
            md5->compare(cstr_md5empty)) {
            string digest;
            MD5HexScan(*md5, digest);
            newdocument.add_value(VALUE_MD5, digest);
            newdocument.add_boolean_term(wrap_prefix("XM") + *md5);
        }

        LOGDEB0("Rcl::Db::add: new doc record:\n" << record << "\n");
        newdocument.set_data(record);
    }
#ifdef IDX_THREADS
    if (m_ndbw->m_havewriteq) {
        DbUpdTask *tp = new DbUpdTask(
            DbUpdTask::AddOrUpdate, udi, uniterm, newdocument_ptr,
            doc.text.length(), rawztext);
        if (!m_ndbw->m_wqueue.put(tp)) {
            LOGERR("Db::addOrUpdate:Cant queue task\n");
            delete newdocument_ptr;
            return false;
        } else {
            return true;
        }
    }
#endif

    return m_ndbw->addOrUpdateWrite(udi, uniterm, newdocument_ptr,
                                    doc.text.length(), rawztext);
}


#ifdef IDX_THREADS
void DbW::waitUpdIdle()
{
    if (m_ndbw->m_iswritable && m_ndbw->m_havewriteq) {
        Chrono chron;
        m_ndbw->m_wqueue.waitIdle();
        // We flush here just for correct measurement of the thread work time
        string ermsg;
        try {
            m_ndbw->xwdb.commit();
        } XCATCHERROR(ermsg);
        if (!ermsg.empty()) {
            LOGERR("Db::waitUpdIdle: flush() failed: " << ermsg << "\n");
        }
        m_ndbw->m_totalworkns += chron.nanos();
        LOGINFO("Db::waitUpdIdle: total xapian work " <<
                lltodecstr(m_ndbw->m_totalworkns/1000000) << " mS\n");
    }
}
#endif

// Flush when idxflushmbs is reached
bool DbW::maybeflush(int64_t moretext)
{
    if (m_flushMb > 0) {
        m_curtxtsz += moretext;
        if ((m_curtxtsz - m_flushtxtsz) / MB >= m_flushMb) {
            LOGINF("Db::add/delete: txt size >= " << m_flushMb <<
                   " Mb, flushing\n");
            return doFlush();
        }
    }
    return true;
}

bool DbW::doFlush()
{
    if (!m_ndbw) {
        LOGERR("Db::doFLush: no ndb??\n");
        return false;
    }
    string ermsg;
    try {
        statusUpdater()->update(DbIxStatus::DBIXS_FLUSH, "");
        m_ndbw->xwdb.commit();
    } XCATCHERROR(ermsg);
    statusUpdater()->update(DbIxStatus::DBIXS_NONE, "");
    if (!ermsg.empty()) {
        LOGERR("Db::doFlush: flush() failed: " << ermsg << "\n");
        return false;
    }
    m_flushtxtsz = m_curtxtsz;
    return true;
}

void DbW::setExistingFlags(const string& udi, unsigned int docid)
{
    if (m_mode == DbRO)
        return;
    if (docid == (unsigned int)-1) {
        LOGERR("Db::setExistingFlags: called with bogus docid !!\n");
        return;
    }
#ifdef IDX_THREADS
    std::unique_lock<std::mutex> lock(m_ndbw->m_mutex);
#endif
    i_setExistingFlags(udi, docid);
}

void DbW::i_setExistingFlags(const string& udi, unsigned int docid)
{
    // Set the up to date flag for the document and its
    // subdocs. needUpdate() can also be called at query time (for
    // preview up to date check), so no error if the updated bitmap is
    // of size 0, and also this now happens when fsIndexer() calls
    // udiTreeMarkExisting() after an error, so the message level is
    // now debug
    if (docid >= m_ndbw->updated.size()) {
        if (m_ndbw->updated.size()) {
            LOGDEB("needUpdate: existing docid beyond updated.size() "
                   "(probably ok). Udi [" << udi << "], docid " << docid <<
                   ", m_ndbw->updated.size() " << m_ndbw->updated.size() << "\n");
        }
        return;
    } else {
        m_ndbw->updated[docid] = true;
    }

    // Set the existence flag for all the subdocs (if any)
    vector<Xapian::docid> docids;
    if (!m_ndbw->subDocs(udi, 0, docids)) {
        LOGERR("Rcl::Db::needUpdate: can't get subdocs\n");
        return;
    }
    for (auto docid : docids) {
        if (docid < m_ndbw->updated.size()) {
            LOGDEB2("Db::needUpdate: docid " << docid << " set\n");
            m_ndbw->updated[docid] = true;
        }
    }
}


/**
 * Delete stem db for given language
 */
bool DbW::deleteStemDb(const string& lang)
{
    LOGDEB("Db::deleteStemDb(" << lang << ")\n");
    if (m_ndbw == 0 || m_ndbw->m_isopen == false || !m_ndbw->m_iswritable)
        return false;
    XapWritableSynFamily db(m_ndbw->xwdb, synFamStem);
    return db.deleteMember(lang);
}

/**
 * Create database of stem to parents associations for a given language.
 * We walk the list of all terms, stem them, and create another Xapian db
 * with documents indexed by a single term (the stem), and with the list of
 * parent terms in the document data.
 */
bool DbW::createStemDbs(const vector<string>& langs)
{
    LOGDEB("Db::createStemDbs\n");
    if (m_ndbw == 0 || m_ndbw->m_isopen == false || !m_ndbw->m_iswritable) {
        LOGERR("createStemDb: db not open or not writable\n");
        return false;
    }

    return createExpansionDbs(m_ndbw->xwdb, langs);
}

/**
 * This is called at the end of an indexing session, to delete the
 * documents for files that are no longer there. This can ONLY be called
 * after a full file-system tree walk, else the file existence flags will 
 * be wrong.
 */
bool DbW::purge()
{
    LOGDEB("Db::purge\n");
    if (m_ndbw == 0)
        return false;
    LOGDEB("Db::purge: m_isopen " << m_ndbw->m_isopen << " m_iswritable " <<
           m_ndbw->m_iswritable << "\n");
    if (m_ndbw->m_isopen == false || m_ndbw->m_iswritable == false) 
        return false;

#ifdef IDX_THREADS
    // If we manage our own write queue, make sure it's drained and closed
    if (m_ndbw->m_havewriteq)
        m_ndbw->m_wqueue.setTerminateAndWait();
    // else we need to lock out other top level threads. This is just
    // a precaution as they should have been waited for by the top
    // level actor at this point
    std::unique_lock<std::mutex> lock(m_ndbw->m_mutex);
#endif // IDX_THREADS

    // For xapian versions up to 1.0.1, deleting a non-existant
    // document would trigger an exception that would discard any
    // pending update. This could lose both previous added documents
    // or deletions. Adding the flush before the delete pass ensured
    // that any added document would go to the index. Kept here
    // because it doesn't really hurt.
    m_reason.clear();
    try {
        statusUpdater()->update(DbIxStatus::DBIXS_FLUSH, "");
        m_ndbw->xwdb.commit();
    } XCATCHERROR(m_reason);
    statusUpdater()->update(DbIxStatus::DBIXS_NONE, "");
    if (!m_reason.empty()) {
        LOGERR("Db::purge: 1st flush failed: " << m_reason << "\n");
        return false;
    }

    // Walk the document array and delete any xapian document whose
    // flag is not set (we did not see its source during indexing).
    int purgecount = 0;
    for (Xapian::docid docid = 1; docid < m_ndbw->updated.size(); ++docid) {
        if (!m_ndbw->updated[docid]) {
            if ((purgecount+1) % 100 == 0) {
                try {
                    CancelCheck::instance().checkCancel();
                } catch(CancelExcept) {
                    LOGINFO("Db::purge: partially cancelled\n");
                    break;
                }
            }

            try {
                if (m_flushMb > 0) {
                    // We use an average term length of 5 for
                    // estimating the doc sizes which is probably not
                    // accurate but gives rough consistency with what
                    // we do for add/update. I should fetch the doc
                    // size from the data record, but this would be
                    // bad for performance.
                    Xapian::termcount trms = m_ndbw->xwdb.get_doclength(docid);
                    maybeflush(trms * 5);
                }
                m_ndbw->deleteDocument(docid);
                LOGDEB("Db::purge: deleted document #" << docid << "\n");
            } catch (const Xapian::DocNotFoundError &) {
                LOGDEB0("Db::purge: document #" << docid << " not found\n");
            } catch (const Xapian::Error &e) {
                LOGERR("Db::purge: document #" << docid << ": " <<
                       e.get_msg() << "\n");
            } catch (...) {
                LOGERR("Db::purge: document #" << docid << ": unknown error\n");
            }
            purgecount++;
        }
    }

    m_reason.clear();
    try {
        statusUpdater()->update(DbIxStatus::DBIXS_FLUSH, "");
        m_ndbw->xwdb.commit();
    } XCATCHERROR(m_reason);
    statusUpdater()->update(DbIxStatus::DBIXS_NONE, "");
    if (!m_reason.empty()) {
        LOGERR("Db::purge: 2nd flush failed: " << m_reason << "\n");
        return false;
    }
    return true;
}

/* Delete document(s) for given unique identifier (doc and descendents) */
bool DbW::purgeFile(const string &udi, bool *existed)
{
    LOGDEB("Db:purgeFile: [" << udi << "]\n");
    if (m_ndbw == 0 || !m_ndbw->m_iswritable)
        return false;

    string uniterm = make_uniterm(udi);
    bool exists = docExists(uniterm);
    if (existed)
        *existed = exists;
    if (!exists)
        return true;

#ifdef IDX_THREADS
    if (m_ndbw->m_havewriteq) {
        string rztxt;
        DbUpdTask *tp = new DbUpdTask(DbUpdTask::Delete, udi, uniterm, 
                                      0, (size_t)-1, rztxt);
        if (!m_ndbw->m_wqueue.put(tp)) {
            LOGERR("Db::purgeFile:Cant queue task\n");
            return false;
        } else {
            return true;
        }
    }
#endif
    /* We get there is IDX_THREADS is not defined or there is no queue */
    return m_ndbw->purgeFileWrite(false, udi, uniterm);
}

/* Delete subdocs with an out of date sig. We do this to purge
   obsolete subdocs during a partial update where no general purge
   will be done */
bool DbW::purgeOrphans(const string &udi)
{
    LOGDEB("Db:purgeOrphans: [" << udi << "]\n");
    if (m_ndbw == 0 || !m_ndbw->m_iswritable)
        return false;

    string uniterm = make_uniterm(udi);

#ifdef IDX_THREADS
    if (m_ndbw->m_havewriteq) {
        string rztxt;
        DbUpdTask *tp = new DbUpdTask(DbUpdTask::PurgeOrphans, udi, uniterm, 
                                      0, (size_t)-1, rztxt);
        if (!m_ndbw->m_wqueue.put(tp)) {
            LOGERR("Db::purgeFile:Cant queue task\n");
            return false;
        } else {
            return true;
        }
    }
#endif
    /* We get there is IDX_THREADS is not defined or there is no queue */
    return m_ndbw->purgeFileWrite(true, udi, uniterm);
}

// Walk an UDI section (all UDIs beginning with input prefix), and
// mark all docs and subdocs as existing. Caller beware: Makes sense
// or not depending on the UDI structure for the data store. In practise,
// used for absent FS mountable volumes.
bool DbW::udiTreeMarkExisting(const string& udi)
{
    LOGDEB("Db::udiTreeMarkExisting: " << udi << endl);
    string wrapd = wrap_prefix(udi_prefix);
    string expr = udi + "*";

#ifdef IDX_THREADS
    std::unique_lock<std::mutex> lock(m_ndbw->m_mutex);
#endif

    bool ret = m_ndbw->idxTermMatch_p(
        int(ET_WILD), cstr_null, expr,
        [this, &udi](const string& term, Xapian::termcount, Xapian::doccount) {
            Xapian::PostingIterator docid;
            XAPTRY(docid = m_ndbw->xrdb.postlist_begin(term), m_ndbw->xrdb,
                   m_reason);
            if (!m_reason.empty()) {
                LOGERR("Db::udiTreeWalk: xapian::postlist_begin failed: " <<
                       m_reason << "\n");
                return false;
            }
            if (docid == m_ndbw->xrdb.postlist_end(term)) {
                LOGDEB("Db::udiTreeWalk:no doc for " << term << " ??\n");
                return false;
            }
            i_setExistingFlags(udi, *docid);
            LOGDEB0("Db::udiTreeWalk: uniterm: " << term << endl);
            return true;
        }, wrapd);
    return ret;
}

} // namespace Rcl
