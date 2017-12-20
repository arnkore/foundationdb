/*
 * AsyncFileBlobStore.actor.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

// When actually compiled (NO_INTELLISENSE), include the generated version of this file.  In intellisense use the source version.
#if defined(NO_INTELLISENSE) && !defined(FDBRPC_ASYNCFILEBLOBSTORE_ACTOR_G_H)
	#define FDBRPC_ASYNCFILEBLOBSTORE_ACTOR_G_H
	#include "AsyncFileBlobStore.actor.g.h"
#elif !defined(FDBRPC_ASYNCFILEBLOBSTORE_ACTOR_H)
	#define FDBRPC_ASYNCFILEBLOBSTORE_ACTOR_H

#include <sstream>
#include <time.h>

#include "IAsyncFile.h"
#include "flow/serialize.h"
#include "flow/Net2Packet.h"
#include "IRateControl.h"
#include "BlobStore.h"
#include "md5/md5.h"
#include "libb64/encode.h"

ACTOR template<typename T> static Future<T> joinErrorGroup(Future<T> f, Promise<Void> p) {
	try {
		Void _ = wait(success(f) || p.getFuture());
		return f.get();
	} catch(Error &e) {
		if(p.canBeSet())
			p.sendError(e);
		throw;
	}
}
// This class represents a write-only file that lives in an S3-style blob store.  It writes using the REST API,
// using multi-part upload and beginning to transfer each part as soon as it is large enough.
// All write operations file operations must be sequential and contiguous.
// Limits on part sizes, upload speed, and concurrent uploads are taken from the BlobStoreEndpoint being used.
class AsyncFileBlobStoreWrite : public IAsyncFile, public ReferenceCounted<AsyncFileBlobStoreWrite> {
public:
	virtual void addref() { ReferenceCounted<AsyncFileBlobStoreWrite>::addref(); }
	virtual void delref() { ReferenceCounted<AsyncFileBlobStoreWrite>::delref(); }

	struct Part : ReferenceCounted<Part> {
		Part(int n) : number(n), writer(content.getWriteBuffer(), NULL, Unversioned()), length(0) {
			etag = std::string();
			::MD5_Init(&content_md5_buf);
		}
		virtual ~Part() {
			etag.cancel();
		}
		Future<std::string> etag;
		int number;
		UnsentPacketQueue content;
		std::string md5string;
		PacketWriter writer;
		int length;
		void write(const uint8_t *buf, int len) {
			writer.serializeBytes(buf, len);
			::MD5_Update(&content_md5_buf, buf, len);
			length += len;
		}
		// MD5 sum can only be finalized once, further calls will do nothing so new writes will be reflected in the sum.
		void finalizeMD5() {
			if(md5string.empty()) {
				std::string sumBytes;
				sumBytes.resize(16);
				::MD5_Final((unsigned char *)sumBytes.data(), &content_md5_buf);
				md5string = base64::encoder::from_string(sumBytes);
				md5string.resize(md5string.size() - 1);
			}
		}

	private:
		MD5_CTX content_md5_buf;
	};

	virtual Future<int> read( void *data, int length, int64_t offset ) { throw file_not_readable(); }

	ACTOR static Future<Void> write_impl(Reference<AsyncFileBlobStoreWrite> f, const uint8_t *data, int length) {
		state Part *p = f->m_parts.back().getPtr();
		// If this write will cause the part to cross the min part size boundary then write to the boundary and start a new part.
		while(p->length + length >= f->m_bstore->knobs.multipart_min_part_size) {
			// Finish off this part
			int finishlen = f->m_bstore->knobs.multipart_min_part_size - p->length;
			p->write((const uint8_t *)data, finishlen);

			// Adjust source buffer args
			length -= finishlen;
			data = (const uint8_t *)data + finishlen;

			// End current part (and start new one)
			Void _ = wait(f->endCurrentPart(f.getPtr(), true));
			p = f->m_parts.back().getPtr();
		}

		p->write((const uint8_t *)data, length);
		return Void();
	}

	virtual Future<Void> write( void const *data, int length, int64_t offset ) {
		if(offset != m_cursor)
			throw non_sequential_op();
		m_cursor += length;

		return m_error.getFuture() || write_impl(Reference<AsyncFileBlobStoreWrite>::addRef(this), (const uint8_t *)data, length);
	}

	virtual Future<Void> truncate( int64_t size ) {
		if(size != m_cursor)
			return non_sequential_op();
		return Void();
	}

	ACTOR static Future<std::string> doPartUpload(AsyncFileBlobStoreWrite *f, Part *p) {
		p->finalizeMD5();
		std::string upload_id = wait(f->getUploadID());
		std::string etag = wait(f->m_bstore->uploadPart(f->m_bucket, f->m_object, upload_id, p->number, &p->content, p->length, p->md5string));
		return etag;
	}

	ACTOR static Future<Void> doFinishUpload(AsyncFileBlobStoreWrite* f) {
		// If there is only 1 part then it has not yet been uploaded so just write the whole file at once.
		if(f->m_parts.size() == 1) {
			Reference<Part> part = f->m_parts.back();
			part->finalizeMD5();
			Void _ = wait(f->m_bstore->writeEntireFileFromBuffer(f->m_bucket, f->m_object, &part->content, part->length, part->md5string));
			return Void();
		}

		// There are at least 2 parts.  End the last part (which could be empty)
		Void _ = wait(f->endCurrentPart(f));

		state BlobStoreEndpoint::MultiPartSetT partSet;
		state std::vector<Reference<Part>>::iterator p;

		// Wait for all the parts to be done to get their ETags, populate the partSet required to finish the object upload.
		for(p = f->m_parts.begin(); p != f->m_parts.end(); ++p) {
			std::string tag = wait((*p)->etag);
			if((*p)->length > 0)  // The last part might be empty and has to be omitted.
				partSet[(*p)->number] = tag;
		}

		// No need to wait for the upload ID here because the above loop waited for all the parts and each part required the upload ID so it is ready
		Void _ = wait(f->m_bstore->finishMultiPartUpload(f->m_bucket, f->m_object, f->m_upload_id.get(), partSet));

		return Void();
	}

	// Ready once all data has been sent AND acknowledged from the remote side
	virtual Future<Void> sync() {
		if(m_cursor == 0)
			throw file_not_writable();

		// Only initiate the finish operation once, and also prevent further writing.
		if(!m_finished.isValid()) {
			m_finished = doFinishUpload(this);
			m_cursor = -1;  // Cause future write attempts to fail
		}

		return m_finished;
	}

	//
	// Flush can't really do what the caller would "want" for a blob store file.  The caller would probably notionally want
	// all bytes written to be at least in transit to the blob store, but that is not very feasible.  The blob store
	// has a minimum size requirement for all but the final part, and parts must be sent with a header that specifies
	// their size.  So in the case of a write buffer that does not meet the part minimum size the part could be sent
	// but then if there is any more data written then that part needs to be sent again in its entirety.  So a client
	// that calls flush often could generate far more blob store write traffic than they intend to.
	virtual Future<Void> flush() { return Void(); }

	virtual Future<int64_t> size() { return m_cursor; }

	virtual Future<Void> readZeroCopy( void** data, int* length, int64_t offset ) {
		TraceEvent(SevError, "ReadZeroCopyNotSupported").detail("FileType", "BlobStoreWrite");
		return platform_error();
	}
	virtual void releaseZeroCopy( void* data, int length, int64_t offset ) {}

	virtual int64_t debugFD() { return -1; }

	virtual ~AsyncFileBlobStoreWrite() {
		m_upload_id.cancel();
		m_finished.cancel();
		m_parts.clear();  // Contains futures
	}

	virtual std::string getFilename() { return m_object; }

private:
	Reference<BlobStoreEndpoint> m_bstore;
	std::string m_bucket;
	std::string m_object;

	int64_t m_cursor;

	Future<std::string> m_upload_id;
	Future<Void> m_finished;
	std::vector<Reference<Part>> m_parts;
	Promise<Void> m_error;
	FlowLock m_concurrentUploads;

	// End the current part and start uploading it, but also wait for a part to finish if too many are in transit.
	ACTOR static Future<Void> endCurrentPart(AsyncFileBlobStoreWrite *f, bool startNew = false) {
		if(f->m_parts.back()->length == 0)
			return Void();

		// Wait for an upload slot to be available
		Void _ = wait(f->m_concurrentUploads.take());

		// Do the upload, and if it fails forward errors to m_error and also stop if anything else sends an error to m_error
		// Also, hold a releaser for the concurrent upload slot while all that is going on.
		f->m_parts.back()->etag = holdWhile(std::shared_ptr<FlowLock::Releaser>(new FlowLock::Releaser(f->m_concurrentUploads, 1)),
									joinErrorGroup(doPartUpload(f, f->m_parts.back().getPtr()), f->m_error)
								  );

		// Make a new part to write to
		if(startNew)
			f->m_parts.push_back(Reference<Part>(new Part(f->m_parts.size() + 1)));

		return Void();
	}

	Future<std::string> getUploadID() {
		if(!m_upload_id.isValid())
			m_upload_id = m_bstore->beginMultiPartUpload(m_bucket, m_object);
		return m_upload_id;
	}

public:
	AsyncFileBlobStoreWrite(Reference<BlobStoreEndpoint> bstore, std::string bucket, std::string object)
		: m_bstore(bstore), m_bucket(bucket), m_object(object), m_cursor(0), m_concurrentUploads(bstore->knobs.concurrent_writes_per_file) {

		// Add first part
		m_parts.push_back(Reference<Part>(new Part(1)));
	}

};


// This class represents a read-only file that lives in an S3-style blob store.  It reads using the REST API.
class AsyncFileBlobStoreRead : public IAsyncFile, public ReferenceCounted<AsyncFileBlobStoreRead> {
public:
	virtual void addref() { ReferenceCounted<AsyncFileBlobStoreRead>::addref(); }
	virtual void delref() { ReferenceCounted<AsyncFileBlobStoreRead>::delref(); }

	virtual Future<int> read( void *data, int length, int64_t offset );

	virtual Future<Void> write( void const *data, int length, int64_t offset ) { throw file_not_writable(); }
	virtual Future<Void> truncate( int64_t size ) { throw file_not_writable(); }

	virtual Future<Void> sync() { return Void(); }
	virtual Future<Void> flush() { return Void(); }

	virtual Future<int64_t> size();

	virtual Future<Void> readZeroCopy( void** data, int* length, int64_t offset ) {
		TraceEvent(SevError, "ReadZeroCopyNotSupported").detail("FileType", "BlobStoreRead");
		return platform_error();
	}
	virtual void releaseZeroCopy( void* data, int length, int64_t offset ) {}

	virtual int64_t debugFD() { return -1; }

	virtual std::string getFilename() { return m_object; }

	virtual ~AsyncFileBlobStoreRead() {}

	Reference<BlobStoreEndpoint> m_bstore;
	std::string m_bucket;
	std::string m_object;
	Future<int64_t> m_size;

	AsyncFileBlobStoreRead(Reference<BlobStoreEndpoint> bstore, std::string bucket, std::string object)
		: m_bstore(bstore), m_bucket(bucket), m_object(object) {
	}

};

#endif
