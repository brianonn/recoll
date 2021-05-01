/* Copyright (C) 2007-2021 J.F.Dockes
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
#ifndef _RCLDBW_P_H_INCLUDED_
#define _RCLDBW_P_H_INCLUDED_

#include "rcldb_p.h"

#ifdef IDX_THREADS
#include "workqueue.h"
#endif // IDX_THREADS


namespace Rcl {

class Query;

#ifdef IDX_THREADS
// Task for the index update thread. This can be 
//  - add/update for a new / update document
//  - delete for a deleted document
//  - purgeOrphans when a multidoc file is updated during a partial pass (no 
//    general purge). We want to remove subDocs that possibly don't
//    exist anymore. We find them by their different sig
// txtlen and doc are only valid for add/update else, len is (size_t)-1 and doc
// is empty
class DbUpdTask {
public:
    enum Op {AddOrUpdate, Delete, PurgeOrphans};
    // Note that udi and uniterm are strictly equivalent and are
    // passed both just to avoid recomputing uniterm which is
    // available on the caller site.
    // Take some care to avoid sharing string data (if string impl is cow)
    DbUpdTask(Op _op, const string& ud, const string& un, 
          Xapian::Document *d, size_t tl, string& rztxt)
        : op(_op), udi(ud.begin(), ud.end()), uniterm(un.begin(), un.end()), 
          doc(d), txtlen(tl) {
        rawztext.swap(rztxt);
    }
    // Udi and uniterm equivalently designate the doc
    Op op;
    string udi;
    string uniterm;
    Xapian::Document *doc;
    // txtlen is used to update the flush interval. It's -1 for a
    // purge because we actually don't know it, and the code fakes a
    // text length based on the term count.
    size_t txtlen;
    string rawztext; // Compressed doc text
};
#endif // IDX_THREADS

class DbW::NativeW : public Db::Native {
public:
    NativeW(Db *db);
    virtual ~NativeW();

    virtual void openWrite(const std::string& dir, Db::OpenMode mode) override;
    virtual void closeWrite() override;
    
    // Final steps of doc update, part which need to be single-threaded
    bool addOrUpdateWrite(const string& udi, const string& uniterm, 
                          Xapian::Document *doc, size_t txtlen, const string& rawztext);

    /** Delete all documents which are contained in the input document, 
     * which must be a file-level one.
     * 
     * @param onlyOrphans if true, only delete documents which have
     * not the same signature as the input. This is used to delete docs
     * which do not exist any more in the file after an update, for
     * example the tail messages after a folder truncation). If false,
     * delete all.
     * @param udi the parent document identifier.
     * @param uniterm equivalent to udi, passed just to avoid recomputing.
     */
    bool purgeFileWrite(bool onlyOrphans, const string& udi, const string& uniterm);
    /** Update existing Xapian document for pure extended attrs change */
    bool docToXdocXattrOnly(TextSplitDb *splitter, const string &udi, 
                Doc &doc, Xapian::Document& xdoc);
    /** Remove all terms currently indexed for field defined by idx prefix */
    bool clearField(Xapian::Document& xdoc, const string& pfx,  Xapian::termcount wdfdec);

    /** Check if term wdf is 0 and remove term if so */
    bool clearDocTermIfWdf0(Xapian::Document& xdoc, const string& term);
    void deleteDocument(Xapian::docid docid) {
        string metareason;
        XAPTRY(xwdb.set_metadata(rawtextMetaKey(docid), string()),
               xwdb, metareason);
        if (!metareason.empty()) {
            LOGERR("deleteDocument: set_metadata error: " <<
                   metareason << "\n");
            // not fatal
        }
        xwdb.delete_document(docid);
    }

    
    DbW  *m_rcldbw{nullptr}; // Parent
    // File existence vector: this is filled during the indexing pass. Any
    // document whose bit is not set at the end is purged
    vector<bool> updated;

#ifdef IDX_THREADS
    WorkQueue<DbUpdTask*> m_wqueue;
    long long  m_totalworkns;
    bool m_havewriteq;
    void maybeStartThreads();
    friend void *DbUpdWorker(void*);
#endif // IDX_THREADS

    // Indexing 
    Xapian::WritableDatabase xwdb;
};

} // namespace Rcl

#endif /* _RCLDBW_P_H_INCLUDED_ */
