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

#ifndef _rcldb_p_h_included_
#define _rcldb_p_h_included_

#include "autoconfig.h"

#include <mutex>
#include <functional>

#include <xapian.h>

#include "xmacros.h"
#include "log.h"

#ifndef XAPIAN_AT_LEAST
// Added in Xapian 1.4.2. Define it here for older versions
#define XAPIAN_AT_LEAST(A,B,C)                                      \
    (XAPIAN_MAJOR_VERSION > (A) ||                                  \
     (XAPIAN_MAJOR_VERSION == (A) &&                                \
      (XAPIAN_MINOR_VERSION > (B) ||                                \
       (XAPIAN_MINOR_VERSION == (B) && XAPIAN_REVISION >= (C)))))
#endif

// Recoll index format version is stored in user metadata. When this change,
// we can't open the db and will have to reindex.
static const string cstr_RCL_IDX_VERSION_KEY("RCL_IDX_VERSION_KEY");
static const string cstr_RCL_IDX_VERSION("1");
static const string cstr_RCL_IDX_DESCRIPTOR_KEY("RCL_IDX_DESCRIPTOR_KEY");
static const string cstr_mbreaks("rclmbreaks");
static const string page_break_term = "XXPG/";

namespace Rcl {

class Query;
class TextSplitDb;

// Some prefixes that we could get from the fields file, but are not going
// to ever change.
extern const string fileext_prefix;
extern const string mimetype_prefix;
extern const string xapday_prefix;
extern const string xapmonth_prefix;
extern const string xapyear_prefix;
extern const string pathelt_prefix;
extern const string parent_prefix;
// Special term to mark documents with children.
extern const string has_children_term;
extern const string unsplitfilename_prefix;
// Synthetic abstract marker (to discriminate from abstract actually
// found in document)
extern const string cstr_syntAbs;
// Special terms to mark begin/end of field (for anchored searches), and
// page breaks
extern string start_of_field_term;
extern string end_of_field_term;
// Empty string md5s 
extern const string cstr_md5empty;
extern const string udi_prefix;

// Compute the unique term used to link documents to their origin. 
// "Q" + external udi
static inline string make_uniterm(const string& udi)
{
    string uniterm(wrap_prefix(udi_prefix));
    uniterm.append(udi);
    return uniterm;
}

// Compute parent term used to link documents to their parent document (if any)
// "F" + parent external udi
static inline string make_parentterm(const string& udi)
{
    // I prefer to be in possible conflict with omega than with
    // user-defined fields (Xxxx) that we also allow. "F" is currently
    // not used by omega (2008-07)
    string pterm(wrap_prefix(parent_prefix));
    pterm.append(udi);
    return pterm;
}

// A class for data and methods that would have to expose
// Xapian-specific stuff if they were in Rcl::Db. There could actually be
// 2 different ones for indexing or query as there is not much in
// common.
class Db::Native {
 public:
    Native(Db *db);
    virtual ~Native();

    virtual void openWrite(const std::string& dir, Db::OpenMode mode) { return;}
    virtual void closeWrite() {}

    void openRead(const string& dir);

    // Determine if an existing index is of the full-text-storing kind
    // by looking at the index metadata. Stores the result in m_storetext
    void storesDocText(Xapian::Database&);
    
    bool getPagePositions(Xapian::docid docid, vector<int>& vpos);
    int getPageNumberForPosition(const vector<int>& pbreaks, int pos);

    bool dbDataToRclDoc(Xapian::docid docid, std::string &data, Doc &doc,
                        bool fetchtext = false);

    size_t whatDbIdx(Xapian::docid id);
    Xapian::docid whatDbDocid(Xapian::docid);

    /** Retrieve Xapian::docid, given unique document identifier, 
     * using the posting list for the derived term.
     * 
     * @param udi the unique document identifier (opaque hashed path+ipath).
     * @param idxi the database index, at query time, when using external
     *     databases.
     * @param[out] xdoc the xapian document.
     * @return 0 if not found
     */
    Xapian::docid getDoc(const string& udi, int idxi, Xapian::Document& xdoc);

    /** Retrieve unique document identifier for given Xapian document, 
     * using the document termlist 
     */
    bool xdocToUdi(Xapian::Document& xdoc, string &udi);

    /** Check if doc is indexed by term */
    bool hasTerm(const string& udi, int idxi, const string& term);

    /** Compute list of subdocuments for a given udi. We look for documents 
     * indexed by a parent term matching the udi, the posting list for the 
     * parentterm(udi)  (As suggested by James Aylett)
     *
     * Note that this is not currently recursive: all subdocs are supposed 
     * to be children of the file doc.
     * Ie: in a mail folder, all messages, attachments, attachments of
     * attached messages etc. must have the folder file document as
     * parent. 
     *
     * Finer grain parent-child relationships are defined by the
     * indexer (rcldb user), using the ipath.
     * 
     */
    bool subDocs(const string &udi, int idxi, vector<Xapian::docid>& docids);

    /** Matcher */
    bool idxTermMatch_p(int typ_sens,const string &lang,const std::string &term,
                        std::function<bool(const std::string& term,
                                           Xapian::termcount colfreq,
                                           Xapian::doccount termfreq)> client,
                        const string& field);

    /** Check if a page position list is defined */
    bool hasPages(Xapian::docid id);

    std::string rawtextMetaKey(Xapian::docid did) {
        // Xapian's Olly Betts avises to use a key which will
        // sort the same as the docid (which we do), and to
        // use Xapian's pack.h:pack_uint_preserving_sort() which is
        // efficient but hard to read. I'd wager that this
        // does not make much of a difference. 10 ascii bytes
        // gives us 10 billion docs, which is enough (says I).
        char buf[30];
        sprintf(buf, "%010d", did);
        return buf;
    }

    bool getRawText(Xapian::docid docid, string& rawtext);

    
    Db  *m_rcldb; // Parent
    bool m_isopen;
    bool m_iswritable;
    bool m_noversionwrite; //Set if open failed because of version mismatch!
    bool m_storetext{false};
#ifdef IDX_THREADS
    std::mutex m_mutex;
#endif

    // Querying (active even if the wdb is too)
    Xapian::Database xrdb;
};

// This is the word position offset at which we index the body text
// (abstract, keywords, etc.. are stored before this)
static const unsigned int baseTextPosition = 100000;

} // namespace Rcl

#endif /* _rcldb_p_h_included_ */
