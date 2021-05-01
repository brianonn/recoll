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
#ifndef _RCLDBW_H_INCLUDED_
#define _RCLDBW_H_INCLUDED_

#include "rcldb.h"

namespace Rcl {

class DbW : public Db {
public:
    DbW(const RclConfig *cfp);
    ~DbW();
    DbW(const DbW &) = delete;
    DbW& operator=(const DbW &) = delete;
    virtual bool createNative() override;

    /** Set the existance flags for the document and its eventual subdocuments
     * 
     * This can be called by the indexer after needUpdate() has returned true,
     * if the indexer does not wish to actually re-index (e.g.: the doc is 
     * known to cause errors).
     */
    void setExistingFlags(const string& udi, unsigned int docid);

    /** Indicate if we are doing a systematic reindex. This complements
        needUpdate() return */
    bool inFullReset() {return o_inPlaceReset || m_mode == DbTrunc;}

    /** Add or update document identified by unique identifier.
     * @param config Config object to use. Can be the same as the member config
     *   or a clone, to avoid sharing when called in multithread context.
     * @param udi the Unique Document Identifier is opaque to us. 
     *   Maximum size 150 bytes.
     * @param parent_udi the UDI for the container document. In case of complex
     *  embedding, this is not always the immediate parent but the UDI for
     *  the container file (which may be a farther ancestor). It is
     *  used for purging subdocuments when a file ceases to exist and
     *  to set the existence flags of all subdocuments of a container
     *  that is found to be up to date. In other words, the
     *  parent_udi is the UDI for the ancestor of the document which
     *  is subject to needUpdate() and physical existence tests (some
     *  kind of file equivalent). Empty for top-level docs. Should
     *  probably be renamed container_udi.
     * @param doc container for document data. Should have been filled as 
     *   much as possible depending on the document type. 
     *   ** doc will be modified in a destructive way **
     */
    bool addOrUpdate(const string &udi, const string &parent_udi, Doc &doc);

#ifdef IDX_THREADS
    void waitUpdIdle();
#endif

    /** Delete document(s) for given UDI, including subdocs */
    bool purgeFile(const string &udi, bool *existed = 0);
    /** Delete subdocs with an out of date sig. We do this to purge
        obsolete subdocs during a partial update where no general purge
        will be done */
    bool purgeOrphans(const string &udi);

    /** Remove documents that no longer exist in the file system. This
     * depends on the update map, which is built during
     * indexing (needUpdate() / addOrUpdate()). 
     *
     * This should only be called after a full walk of
     * the file system, else the update map will not be complete, and
     * many documents will be deleted that shouldn't, which is why this
     * has to be called externally, rcldb can't know if the indexing
     * pass was complete or partial.
     */
    bool purge();

    /** Create stem expansion database for given languages. */
    bool createStemDbs(const std::vector<std::string> &langs);
    /** Delete stem expansion database for given language. */
    bool deleteStemDb(const string &lang);

    bool doFlush();

    // Mark all documents with an UDI having input as prefix as
    // existing.  Only works if the UDIs for the store are
    // hierarchical of course.  Used by FsIndexer to avoid purging
    // files for a topdir which is on a removable file system and
    // currently unmounted (topdir does not exist or is empty.
    bool udiTreeMarkExisting(const string& udi);

    // A place for things we don't want visible here.
    class NativeW;
    friend class NativeW;
    /* This has to be public for access by embedded Query::Native */
    NativeW *m_ndbw{nullptr};

private:
    // Text bytes indexed since beginning
    long long    m_curtxtsz{0};
    // Text bytes at last flush
    long long    m_flushtxtsz{0};
    // Text bytes at last fsoccup check
    long long    m_occtxtsz{0};
    // First fs occup check ?
    int         m_occFirstCheck{1};
#ifdef IDX_THREADS
    friend void *DbUpdWorker(void*);
#endif // IDX_THREADS

    // Internal form of setExistingFlags: no locking
    virtual void i_setExistingFlags(const string& udi, unsigned int docid) override;
    // Flush when idxflushmb is reached
    bool maybeflush(int64_t moretext);
};

} // namespace Rcl

#endif /* _RCLDBW_H_INCLUDED_ */
