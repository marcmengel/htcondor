/***************************************************************
 *
 * Copyright (C) 1990-2007, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 * 
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/


#include "condor_common.h"
#include "authentication.h"
#include "condor_debug.h"
#include "condor_config.h"
#include "internet.h"
#include "condor_rw.h"
#include "condor_md.h"
#include "selector.h"
#include "ccb_client.h"
#include "condor_sockfunc.h"
#include "condor_crypt_aesgcm.h"

#define NORMAL_HEADER_SIZE 5
#define MAX_HEADER_SIZE MAC_SIZE + NORMAL_HEADER_SIZE

#define MAX_MESSAGE_SIZE (1024*1024)

/**************************************************************/

/* 
   NOTE: All ReliSock constructors initialize with this, so you can
   put any shared initialization code here.  -Derek Wright 3/12/99
*/
void
ReliSock::init()
{
	m_auth_in_progress = false;
	m_authob = NULL;
	m_has_backlog = false;
	m_read_would_block = false;
	m_non_blocking = false;
	ignore_next_encode_eom = FALSE;
	ignore_next_decode_eom = FALSE;
	_bytes_sent = 0.0;
	_bytes_recvd = 0.0;
	_special_state = relisock_none;
	is_client = 0;
	hostAddr = NULL;
	statsBuf = NULL;
	snd_msg.reset();
	rcv_msg.reset();
	rcv_msg.init_parent(this);
	snd_msg.init_parent(this);
	m_target_shared_port_id = NULL;
	m_finished_recv_header = false;
	m_finished_send_header = false;
	m_final_send_header = false;
	m_final_recv_header = false;
}


ReliSock::ReliSock()
	: Sock(),
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	m_send_md_ctx(nullptr, EVP_MD_CTX_destroy),
	m_recv_md_ctx(nullptr, EVP_MD_CTX_destroy)
#else
	m_send_md_ctx(nullptr, EVP_MD_CTX_free),
	m_recv_md_ctx(nullptr, EVP_MD_CTX_free)
#endif
{
	init();
}

ReliSock::ReliSock(const ReliSock & orig) : Sock(orig),
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
	m_send_md_ctx(nullptr, EVP_MD_CTX_destroy),
	m_recv_md_ctx(nullptr, EVP_MD_CTX_destroy)
#else
	m_send_md_ctx(nullptr, EVP_MD_CTX_free),
	m_recv_md_ctx(nullptr, EVP_MD_CTX_free)
#endif
{
	init();
	// now copy all cedar state info via the serialize() method
	char *buf = NULL;
	buf = orig.serialize();	// get state from orig sock
	ASSERT(buf);
	serialize(buf);			// put the state into the new sock
	delete [] buf;
}

Stream *
ReliSock::CloneStream()
{
	return new ReliSock(*this);
}

ReliSock::~ReliSock()
{
	close();
	if ( m_authob ) {
		delete m_authob;
		m_authob = NULL;
	}
	if ( hostAddr ) {
		free( hostAddr );
		hostAddr = NULL;
	}
	if (statsBuf) {
		free(statsBuf);
		statsBuf = NULL;
	}
	if( m_target_shared_port_id ) {
		free( m_target_shared_port_id );
		m_target_shared_port_id = NULL;
	}
}

int
ReliSock::close()
{
	// Purge send and receive buffers at the relisock level
	snd_msg.reset();
	rcv_msg.reset();
	m_finished_send_header = false;
	m_finished_recv_header = false;
	m_final_send_header = false;
	m_final_recv_header = false;
	m_send_md_ctx.reset();
	m_recv_md_ctx.reset();

	// then invoke close() in parent class to close fd etc
	return Sock::close();
}

int 
ReliSock::listen()
{
	if (_state != sock_bound) {
        dprintf(D_ALWAYS, "Failed to listen on TCP socket, because it is not bound to a port.\n");
        return FALSE;
    }

	// Ask for a (configurable) large backlog of connections. If this
	// value is too large, the OS will cap it at the kernel's current
	// maxiumum. Why not just use SOMAXCONN? Unfortunately, it's a
	// fairly small value (128) on many platforms.
	if( ::listen( _sock, param_integer( "SOCKET_LISTEN_BACKLOG", 500 ) ) < 0 ) {

            char const *self_address = get_sinful();
            if( !self_address ) {
                self_address = "<bad address>";
            }
#ifdef WIN32
			int error = WSAGetLastError();
			dprintf( D_ALWAYS, "Failed to listen on TCP socket %s: WSAError = %d\n", self_address, error );
#else
			dprintf(D_ALWAYS, "Failed to listen on TCP socket %s: (errno = %d) %s\n", self_address, errno, strerror(errno));
#endif

			return FALSE;
	}

	dprintf( D_NETWORK, "LISTEN %s fd=%d\n", sock_to_string(_sock),
			 _sock );

	_state = sock_special;
	_special_state = relisock_listen;

	return TRUE;
}


/// FALSE means this is an incoming connection
int ReliSock::listen(condor_protocol proto, int port)
{
	if (!bind(proto, false, port, false)) return
		FALSE;
	return listen();
}

int 
ReliSock::accept( ReliSock	&c )
{
	int c_sock;

	if (_state != sock_special || _special_state != relisock_listen ||
													c._state != sock_virgin)
	{
		return FALSE;
	}

	if (_timeout > 0) {
		Selector		selector;
		selector.set_timeout( _timeout );
		selector.add_fd( _sock, Selector::IO_READ );

		selector.execute();

		if( selector.timed_out() ) {
			return FALSE;
		} else if ( !selector.has_ready() ) {
			dprintf( D_ALWAYS, "select returns %d, connect failed\n",
				selector.select_retval() );
			return FALSE;
		}
	}

#ifndef WIN32 /* Unix */
	errno = 0;
#endif
	if ((c_sock = condor_accept(_sock, c._who)) < 0) {
#ifndef WIN32 /* Unix */
		if ( errno == EMFILE ) {
			_condor_fd_panic ( __LINE__, __FILE__ ); /* This calls dprintf_exit! */
		}
#endif
		return FALSE;

	}

	c.assignSocket(c_sock);
	c.enter_connected_state("ACCEPT");
	c.decode();

	c.set_keepalive();

		/* Set no delay to disable Nagle, since we buffer all our
		   relisock output and it degrades performance of our
		   various chatty protocols. -Todd T, 9/05
		*/
	int on = 1;
	c.setsockopt(IPPROTO_TCP, TCP_NODELAY, (char*)&on, sizeof(on));

	return TRUE;
}


int 
ReliSock::accept( ReliSock	*c)
{
	if (!c) 
	{
		return FALSE;
	}

	return accept(*c);
}

bool ReliSock :: set_encryption_id(const char * /* keyId */)
{
    return false; // TCP does not need this yet
}

bool ReliSock::init_MD(CONDOR_MD_MODE mode, KeyInfo * key, const char * /* keyId */)
{
    return (snd_msg.init_MD(mode, key) && rcv_msg.init_MD(mode, key));
}

ReliSock *
ReliSock::accept()
{
	ReliSock	*c_rs;
	int c_sock;

	if (!(c_rs = new ReliSock())) {
		return (ReliSock *)0;
	}

	if ((c_sock = accept(*c_rs)) == FALSE) {
		delete c_rs;
		return (ReliSock *)0;
	}

	return c_rs;
}

int 
ReliSock::connect( char	const *host, int port, bool non_blocking_flag )
{
	if (hostAddr != NULL)
	{
		free(hostAddr);
		hostAddr = NULL;
	}
 
	init();     
	is_client = 1;
	if( ! host ) {
		return FALSE;
	}
	hostAddr = strdup( host );
	return do_connect( host, port, non_blocking_flag );
}

int 
ReliSock::put_line_raw( const char *buffer )
{
	int result;
	int length = strlen(buffer);
	result = put_bytes_raw(buffer,length);
	if(result!=length) return -1;
	result = put_bytes_raw("\n", 1);
	if(result!=1) return -1;
	return length;
}

int
ReliSock::get_line_raw( char *buffer, int length )
{
	int total=0;
	int actual;

	while( length>0 ) {
		actual = get_bytes_raw(buffer,1);
		if(actual<=0) break;
		if(*buffer=='\n') break;

		buffer++;
		length--;
		total++;
	}
	
	*buffer = 0;
	return total;
}

int 
ReliSock::put_bytes_raw( const char *buffer, int length )
{
	return condor_write(peer_description(),_sock,buffer,length,_timeout);
}

int 
ReliSock::get_bytes_raw( char *buffer, int length )
{
	return condor_read(peer_description(),_sock,buffer,length,_timeout);
}

int 
ReliSock::put_bytes_nobuffer( const char *buffer, int length, int send_size )
{
	int i, result, l_out;
	int pagesize = 65536;  // Optimize large writes to be page sized.
	const char * cur;
	unsigned char * buf = NULL;

	if (crypto_state_ && crypto_state_->m_keyInfo.getProtocol() == CONDOR_AESGCM) {
		dprintf(D_ALWAYS, "ReliSock::put_bytes_nobuffer is not allowed with AES encryption, failing\n");
		return -1;
	}

	// First, encrypt the data if necessary
	if (get_encryption()) {
		if (!wrap((const unsigned char *) buffer, length,  buf , l_out)) {
			dprintf(D_SECURITY, "Encryption failed\n");
			goto error;
		}
		cur = (char *)buf;
	}
	else {
		cur = buffer;
	}

	// Tell peer how big the transfer is going to be, if requested.
	// Note: send_size param is 1 (true) by default.
	this->encode();
	if ( send_size ) {
		ASSERT( this->code(length) != FALSE );
		ASSERT( this->end_of_message() != FALSE );
	}

	// First drain outgoing buffers
	if ( !prepare_for_nobuffering(stream_encode) ) {
		// error flushing buffers; error message already printed
            goto error;
	}

	// Optimize transfer by writing in pagesized chunks.
	for(i = 0; i < length;)
	{
		// If there is less then a page left.
		if( (length - i) < pagesize ) {
			result = condor_write(peer_description(), _sock, cur, (length - i), _timeout);
			if( result < 0 ) {
                                goto error;
			}
			cur += (length - i);
			i += (length - i);
		} else {  
			// Send another page...
			result = condor_write(peer_description(), _sock, cur, pagesize, _timeout);
			if( result < 0 ) {
                            goto error;
			}
			cur += pagesize;
			i += pagesize;
		}
	}
	if (i > 0) {
		_bytes_sent += i;
	}
        
        free(buf);

	return i;
 error:
        dprintf(D_ALWAYS, "ReliSock::put_bytes_nobuffer: Send failed.\n");

        free(buf);

        return -1;
}

int 
ReliSock::get_bytes_nobuffer(char *buffer, int max_length, int receive_size)
{
	int result;
	int length;
    unsigned char * buf = NULL;

	ASSERT(buffer != NULL);
	ASSERT(max_length > 0);

	if (crypto_state_ && crypto_state_->m_keyInfo.getProtocol() == CONDOR_AESGCM) {
		dprintf(D_ALWAYS, "ReliSock::get_bytes_nobuffer is not allowed with AES encryption, failing\n");
		return -1;
	}

	// Find out how big the file is going to be, if requested.
	// No receive_size means read max_length bytes.
	this->decode();
	if ( receive_size ) {
		ASSERT( this->code(length) != FALSE );
		ASSERT( this->end_of_message() != FALSE );
	} else {
		length = max_length;
	}

	// First drain incoming buffers
	if ( !prepare_for_nobuffering(stream_decode) ) {
		// error draining buffers; error message already printed
            goto error;
	}


	if( length > max_length ) {
		dprintf(D_ALWAYS, 
			"ReliSock::get_bytes_nobuffer: data too large for buffer.\n");
                goto error;
	}

	result = condor_read(peer_description(), _sock, buffer, length, _timeout);

	
	if( result < 0 ) {
		dprintf(D_ALWAYS, 
			"ReliSock::get_bytes_nobuffer: Failed to receive file.\n");
                goto error;
	} 
	else {
		// See if it needs to be decrypted
		if (get_encryption()) {
			unwrap((unsigned char *) buffer, result, buf, length);  // I am reusing length
			memcpy(buffer, buf, result);
			free(buf);
		}
		_bytes_recvd += result;
		return result;
	}
 error:
        return -1;
}


int 
ReliSock::handle_incoming_packet()
{
	/* if socket is listening, and packet is there, it is ready for accept */
	if (_state == sock_special && _special_state == relisock_listen) {
		return TRUE;
	}

		// since we are trying to read from the socket, we can assume
		// that it is no longer ok for there to be no message at all.
	allow_empty_message_flag = FALSE;

	/* do not queue up more than one message at a time on reliable sockets */
	/* but return 1, because old message can still be read.						*/
	if (rcv_msg.ready) {
		return TRUE;
	}

	return rcv_msg.rcv_packet(peer_description(), _sock, _timeout);
}

int
ReliSock::finish_end_of_message()
{
	dprintf(D_NETWORK, "Finishing a non-blocking EOM.\n");
	BlockingModeGuard guard(this, true);
	int retval;
	if (snd_msg.buf.num_used())
	{
		retval = snd_msg.snd_packet(peer_description(), _sock, true, _timeout);
	}
	else
	{
		retval = snd_msg.finish_packet(peer_description(), _sock, _timeout);
	}
	if (retval == 3 || retval == 2) m_has_backlog = true;
	return retval;
}

	// Ret values:
	// - TRUE: successfully sent EOM; if set to ignore next EOM, we did nothing.
	// - FALSE: failure occurred when sending EOM.
	// - 2: When encoding mode and non-blocking is enabled, this indicates that
	//   a subset of the data successfully went to the wire but finish_end_of_message
	//   needs to be called.
	// If allow_empty_message is set and there are no bytes to send, return TRUE.
	// Otherwise, an error occurs if there is no unbuffer bytes.
int
ReliSock::end_of_message_nonblocking()
{
	BlockingModeGuard guard(this, true);
	return end_of_message_internal();
}

int
ReliSock::end_of_message()
{
	BlockingModeGuard guard(this, false);
	return end_of_message_internal();
}

int 
ReliSock::end_of_message_internal()
{
	int ret_val = FALSE;

	if (crypto_state_ && crypto_state_->m_keyInfo.getProtocol() != CONDOR_AESGCM) {
		resetCrypto();
	}
	switch(_coding){
		case stream_encode:
			if ( ignore_next_encode_eom == TRUE ) {
				ignore_next_encode_eom = FALSE;
				return TRUE;
			}
			if (!snd_msg.buf.empty()) {
				int retval = snd_msg.snd_packet(peer_description(), _sock, TRUE, _timeout);
				if (retval == 2 || retval == 3) {
					m_has_backlog = true;
				}
				return retval ? true : false;
			}
			if ( allow_empty_message_flag ) {
				allow_empty_message_flag = FALSE;
				return TRUE;
			}
			break;

		case stream_decode:
			if ( ignore_next_decode_eom == TRUE ) {
				ignore_next_decode_eom = FALSE;
				return TRUE;
			}
			if ( rcv_msg.ready ) {
				if ( rcv_msg.buf.consumed() ) {
					ret_val = TRUE;
				}
				else {
					char const *ip = get_sinful_peer();
					dprintf(D_FULLDEBUG,"Failed to read end of message from %s; %d untouched bytes.\n",ip ? ip : "(null)", rcv_msg.buf.num_untouched());
				}
				rcv_msg.ready = FALSE;
				rcv_msg.buf.reset();
			}
			else if ( allow_empty_message_flag ) {
				allow_empty_message_flag = FALSE;
				return TRUE;
			}
			allow_empty_message_flag = FALSE;
			break;

		default:
			ASSERT(0);
	}

	return ret_val;
}

bool
ReliSock::peek_end_of_message()
{
	if ( rcv_msg.ready ) {
		if ( rcv_msg.buf.consumed() ) {
			return true;
		}
	}
	return false;
}

const char * ReliSock :: isIncomingDataHashed()
{
    return NULL;    // For now
}

int 
ReliSock::put_bytes(const void *data, int sz)
{
        // Check to see if we need to encrypt
        // Okay, this is a bug! H.W. 9/25/2001

        if (get_encryption() && crypto_state_->m_keyInfo.getProtocol() != CONDOR_AESGCM) {
        	unsigned char * dta = NULL;
			int l_out;
            if (!wrap((const unsigned char *)(data), sz, dta , l_out)) {
                dprintf(D_SECURITY, "Encryption failed\n");
				if (dta != NULL)
				{
					free(dta);
					dta = NULL;
				}
                return -1;  // encryption failed!
            }
			int r = put_bytes_after_encryption(dta, sz); // l_out instead?
			free(dta);
			return r;
        }
        else {
			// The bytes aren't encrypted at all, just pass through
			return put_bytes_after_encryption(data, sz);
        }
}

int 
ReliSock::put_bytes_after_encryption(const void *dta, int sz) {
	ignore_next_encode_eom = FALSE;

	int		nw;
	int 	tw = 0;
	int		header_size = isOutgoing_Hash_on() ? MAX_HEADER_SIZE:NORMAL_HEADER_SIZE;
	for(nw=0;;) {
		
		if (snd_msg.buf.full()) {
			int retval = snd_msg.snd_packet(peer_description(), _sock, FALSE, _timeout);
			// This would block and the user asked us to work non-buffered - force the
			// buffer to grow to hold the data for now.
			if (retval == 3) {
				nw += snd_msg.buf.put_force(&((const char *)dta)[nw], sz-nw);
				m_has_backlog = true;
				break;
			} else if (!retval) {
				return FALSE;
			}
		}
		
		if (snd_msg.buf.empty()) {
			snd_msg.buf.seek(header_size);
		}
		
		if (dta && (tw = snd_msg.buf.put_max(&((const char *)dta)[nw], sz-nw)) < 0) {
			return -1;
		}
		
		nw += tw;
		if (nw >= sz) {
			break;
		}
	}
	if (nw > 0) {
		_bytes_sent += nw;
	}

	return nw;
}


int 
ReliSock::get_bytes(void *dta, int max_sz)
{
	int		bytes, length;
    unsigned char * data = 0;

	ignore_next_decode_eom = FALSE;

	m_read_would_block = false;
	while (!rcv_msg.ready) {
		int retval = handle_incoming_packet();
		if (retval == 2) {
			dprintf(D_NETWORK, "get_bytes would have blocked - failing call.\n");
			m_read_would_block = true;
			return false;
		} else if (!retval) {
			return FALSE;
		}
	}

	bytes = rcv_msg.buf.get(dta, max_sz);

	if (bytes > 0) {
            if (get_encryption() && crypto_state_->m_keyInfo.getProtocol() != CONDOR_AESGCM) {
                unwrap((unsigned char *) dta, bytes, data, length);
                memcpy(dta, data, bytes);
                free(data);
            }
            _bytes_recvd += bytes;
        }
        
	return bytes;
}


int ReliSock::get_ptr( void *&ptr, char delim)
{
	while (!rcv_msg.ready){
		if (!handle_incoming_packet()) {
			return FALSE;
		}
	}

	return rcv_msg.buf.get_tmp(ptr, delim);
}


int ReliSock::peek( char &c)
{
	while (!rcv_msg.ready) {
		if (!handle_incoming_packet()) {
			return FALSE;
		}
	}

	return rcv_msg.buf.peek(c);
}

bool ReliSock::RcvMsg::init_MD(CONDOR_MD_MODE mode, KeyInfo * key)
{
    if (!buf.consumed()) {
        return false;
    }

    mode_ = mode;
    delete mdChecker_;
	mdChecker_ = 0;

    // only instantiate if we have a key and will use it.  unlike encryption,
    // which previously could be turned on/off during a stream, we either turn
    // MD on at the setup of a stream and leave it on, or we never turn it on
    // at all.
    if (key && (mode != MD_OFF)) {
        mdChecker_ = new Condor_MD_MAC(key);
    }

    return true;
}

void ReliSock::resetHeaderMD()
{
	dprintf(D_NETWORK, "Reset Header MD.\n");
	m_send_md_ctx.reset();
	m_finished_send_header = false;
	m_final_send_header = false;
	m_recv_md_ctx.reset();
	m_finished_recv_header = false;
	m_final_recv_header = false;
}

ReliSock::RcvMsg :: RcvMsg() : 
    mode_(MD_OFF),
    mdChecker_(0), 
	p_sock(0),
	m_partial_packet(false),
	m_remaining_read_length(0),
	m_len_t(0),
	m_end(0),
	m_tmp(NULL),
	ready(0),
	m_closed(false)
{
	memset( m_partial_cksum, 0, sizeof(m_partial_cksum) );
}

ReliSock::RcvMsg::~RcvMsg()
{
    delete mdChecker_;
}

void ReliSock::RcvMsg::reset()
{
	buf.reset();
}


int ReliSock::RcvMsg::rcv_packet( char const *peer_description, SOCKET _sock, int _timeout)
{
	char	        hdr[MAX_HEADER_SIZE];
	char *cksum_ptr = &hdr[5];
	int		len, len_t, header_size, header_filled;
	int		tmp_len;
	int		retval;
	const int max_packet_size = 1024 * 1024;  // We will reject packets bigger than this
	header_size = (mode_ != MD_OFF) ? MAX_HEADER_SIZE : NORMAL_HEADER_SIZE;

	// We read the partial packet in a previous read; try to finish it and
	// then skip down to packet verification.
	if (m_partial_packet) {
		m_partial_packet = false;
		len = m_remaining_read_length;
		cksum_ptr = m_partial_cksum;
		hdr[0] = (char) m_end;
		memcpy(hdr + 1, &m_len_t, 4);
		goto read_packet;
	}

	header_filled = 0;

	retval = condor_read(peer_description,_sock,hdr,header_size,_timeout, 0, p_sock->is_non_blocking());
	if ( retval == 0 ) {   // 0 means that the read would have blocked; unlike a normal read(), condor_read
	                       // returns -2 if the socket has been closed.
		dprintf(D_NETWORK, "Reading header would have blocked.\n");
		return 2;
	}

	// Block on short reads for the header.  Since the header is very short (typically, 5 bytes),
	// we don't care to gracefully handle the case where it has been fragmented over multiple
	// TCP packets.  If in non-blocking mode, we want to limit the wait to to a maximum of 1 second.
	// This is larger than the contractual delay of 'never block' but we need that header to proceeed.
	if ( (retval > 0) && (retval != header_size) ) {

		// if we got some data, check to see if it looks valid, otherwise don't even bother to
		// do a blocking read to get the rest of the header.
		// Network byte order of a 32 bit integer (BigEndian) will have the most significant
		// byte first.  so we can do an *approximate* size validation check if we have
		// at least the first network byte of the size
		// We do this validation because some government labs require a port scanner to be running. If the port
		// scanner sends more than 0 bytes but less than header_size we can block below until the scanner closes the socket.
		for (int ii = retval; ii < 5; ++ii) { hdr[ii] = 0; }
		memcpy(&len_t, &hdr[1], 4);
		len = (int)ntohl(len_t);
		m_end = (int) ((char *)hdr)[0];
		if (m_end < 0 || m_end > 10 || len < 0 || len > max_packet_size) {
			header_filled = retval;
			goto check_header; // jump down to a check we now know will fail
		}

		dprintf(D_NETWORK, "Force-reading remainder of header.\n");
		retval = condor_read(peer_description, _sock, hdr+retval, header_size-retval,
			p_sock->is_non_blocking() ? 1 : _timeout);
	}

	if ( retval < 0 && 
		 retval != -2 ) // -2 means peer just closed the socket
	{
		dprintf(D_ALWAYS,"IO: Failed to read packet header\n");
		return FALSE;
	}
	if ( retval == -2 ) {	// -2 means peer just closed the socket
		dprintf(D_FULLDEBUG,"IO: EOF reading packet header\n");
		m_closed = true;
		return FALSE;
	}

	m_end = (int) ((char *)hdr)[0];
	memcpy(&m_len_t,  &hdr[1], 4);
	len = (int) ntohl(m_len_t);
	header_filled = header_size;

check_header:
	if (m_end < 0 || m_end > 10) {
		char hex[3 * NORMAL_HEADER_SIZE + 1];
		dprintf(D_ALWAYS,"IO: Incoming packet header unrecognized : %s\n",
				debug_hex_dump(hex, &hdr[0], MIN(NORMAL_HEADER_SIZE, header_filled)));
		return FALSE;
	}

	if (len > max_packet_size) {
		char hex[3 * NORMAL_HEADER_SIZE + 1];
		dprintf(D_ALWAYS, "IO: Incoming packet is larger than 1MB limit (requested size %d) : %s\n", len,
				debug_hex_dump(hex, &hdr[0], MIN(NORMAL_HEADER_SIZE, header_filled)));
		return FALSE;
	}

	if (len <= 0) {
		char hex[3 * NORMAL_HEADER_SIZE + 1];
		dprintf(D_ALWAYS, "IO: Incoming packet improperly sized (len=%d,end=%d) : %s\n", len, m_end,
			debug_hex_dump(hex, &hdr[0], MIN(NORMAL_HEADER_SIZE, header_filled)));
		return FALSE;
	}

	if (!(m_tmp = new Buf)){
		dprintf(D_ALWAYS, "IO: Out of memory\n");
		return FALSE;
	}
	m_tmp->grow_buf(len+1);

        if (!p_sock->get_encryption() && !p_sock->m_finished_recv_header && p_sock->_bytes_recvd < MAX_MESSAGE_SIZE) {
                if (!p_sock->m_recv_md_ctx) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
                        p_sock->m_recv_md_ctx.reset(EVP_MD_CTX_create());
#else
                        p_sock->m_recv_md_ctx.reset(EVP_MD_CTX_new());
#endif
                        if (!p_sock->m_recv_md_ctx) {
                                dprintf(D_ALWAYS, "IO: Failed to create a new MD context.\n");
                                return false;
                        }
                        if (1 != EVP_DigestInit_ex(p_sock->m_recv_md_ctx.get(), EVP_sha256(), NULL)) {
                                dprintf(D_ALWAYS, "IO: Failed to initialize SHA-256 context.\n");
                                return false;
                        }
                }
		if (1 != EVP_DigestUpdate(p_sock->m_recv_md_ctx.get(), hdr, header_size)) {
			dprintf(D_ALWAYS, "IO: Failed to update the message digest.\n");
			return false;
		}
		dprintf(D_NETWORK|D_VERBOSE, "AESGCM: Recv header digest added %u bytes \n", header_size);
	}

read_packet:
	dprintf(D_NETWORK|D_VERBOSE, "Reading packet body of length %d\n", len);
	tmp_len = m_tmp->read(peer_description, _sock, len, _timeout, p_sock->is_non_blocking());
	if (tmp_len != len) {
		if (p_sock->is_non_blocking() && (tmp_len >= 0)) {
			m_partial_packet = true;
			m_remaining_read_length = len - tmp_len;
			if ( mode_ != MD_OFF && cksum_ptr != m_partial_cksum ) {
				memcpy( m_partial_cksum, cksum_ptr, sizeof(m_partial_cksum) );
			}
			return 2;
		} else {
			delete m_tmp;
			m_tmp = NULL;
			dprintf(D_ALWAYS, "IO: Packet read failed: read %d of %d\n",
					tmp_len, len);
			return FALSE;
		}
	}

		// Note that we don't check the header here as the body may be
		// split across several function invocations.
	if (!p_sock->get_encryption() && !p_sock->m_finished_recv_header && p_sock->m_recv_md_ctx && (p_sock->_bytes_recvd < 1024*1024)) {
		if (1 != EVP_DigestUpdate(p_sock->m_recv_md_ctx.get(), m_tmp->get_ptr(), m_tmp->num_untouched())) {
			dprintf(D_ALWAYS, "IO: Failed to update the message digest.\n");
			return false;
	        }
		else {
			dprintf(D_NETWORK|D_VERBOSE, "AESGCM: Recv body digest added %u bytes \n", m_tmp->num_untouched());
		}
	}

	if (p_sock->get_encryption() && p_sock->get_crypto_state()->m_keyInfo.getProtocol() == CONDOR_AESGCM) {
		int length = m_tmp->num_untouched();
		Buf new_buf(p_sock, length);
		new_buf.alloc_buf();
		unsigned char *aad_data = reinterpret_cast<unsigned char *>(hdr);
		int aad_len = header_size;
		std::vector<unsigned char> aad_with_digest;
		if (!p_sock->m_finished_recv_header) {
			p_sock->m_finished_recv_header = true;
			unsigned md_size = EVP_MD_size(EVP_sha256());
			aad_len = 2*md_size + header_size;
			aad_with_digest.resize(aad_len,0);
			aad_data = &aad_with_digest[0];
			if (!p_sock->m_final_recv_header) {
				if (p_sock->m_recv_md_ctx &&
					(1 != EVP_DigestFinal_ex(p_sock->m_recv_md_ctx.get(), aad_data, &md_size)))
				{
					dprintf(D_ALWAYS, "IO: Failed to compute final received message digest.\n");
					return false;
				} else if (!p_sock->m_recv_md_ctx) {
					memset(aad_data, '\0', md_size);
					dprintf(D_NETWORK|D_VERBOSE, "Setting first digest in AAD to %u 0's\n", md_size);
				} else {
					dprintf(D_NETWORK|D_VERBOSE, "Successfully set first digest in AAD\n");
				}
				p_sock->m_final_recv_header = true;
				p_sock->m_final_mds.resize(2*md_size,0);
				memcpy(&p_sock->m_final_mds[0] + md_size, aad_data, md_size);
			} else {
				memcpy(aad_data, &p_sock->m_final_mds[0] + md_size, md_size);
			}
			if (!p_sock->m_final_send_header) {
				if (p_sock->m_send_md_ctx &&
					(1 != EVP_DigestFinal_ex(p_sock->m_send_md_ctx.get(), aad_data + md_size, &md_size)))
				{
					dprintf(D_ALWAYS, "IO: Failed to compute final send message digest.\n");
					return false;
				} else if (!p_sock->m_send_md_ctx) {
					memset(aad_data + md_size, '\0', md_size);
					dprintf(D_NETWORK|D_VERBOSE, "Setting second digest in AAD to %u 0's\n", md_size);
				} else {
					dprintf(D_NETWORK|D_VERBOSE, "Successfully set second digest in AAD\n");
				}
				p_sock->m_final_send_header = true;
				p_sock->m_final_mds.resize(2*md_size,0);
				memcpy(&p_sock->m_final_mds[0], aad_data + md_size, md_size);
			} else {
				memcpy(aad_data + md_size, &p_sock->m_final_mds[0], md_size);
			}
			memcpy(aad_data + 2*md_size, hdr, header_size);
			char hex[3*(32*2 + 5) + 1];
			dprintf(D_NETWORK, "Expecting AAD with handshake digest %s\n",
				debug_hex_dump(hex, reinterpret_cast<char*>(aad_data), 32*2 + 5));
		}

		if ( ! ((Condor_Crypt_AESGCM*)p_sock->get_crypto())->decrypt(
                        p_sock->crypto_state_,
			aad_data,
			aad_len,
			static_cast<unsigned char *>(m_tmp->get_ptr()),
			m_tmp->num_untouched(),
			static_cast<unsigned char *>(new_buf.get_ptr()),
			length))
		{
			dprintf(D_ALWAYS, "IO: Failed to unwrap the packet.\n");
			return false;
		}
		m_tmp->swap(new_buf);
		m_tmp->truncate(length);
	}

		// For non-AES-GCM encryption or unexpectedly large headers, release the memory...
	if (p_sock->m_recv_md_ctx && ((p_sock->get_encryption() && p_sock->get_crypto_state()->m_keyInfo.getProtocol() != CONDOR_AESGCM) ||
		(p_sock->m_finished_recv_header && p_sock->m_finished_send_header) || p_sock->_bytes_sent > 1024*1024))
	{
		p_sock->m_finished_recv_header = true;
		p_sock->m_recv_md_ctx.reset();
		dprintf(D_NETWORK, "Resetting Header for recv.\n");
	}

        // Now, check MD
        if (mode_ != MD_OFF) {
            if (!m_tmp->verifyMD(cksum_ptr, mdChecker_)) {
                delete m_tmp;
		m_tmp = NULL;
                dprintf(D_ALWAYS, "IO: Message Digest/MAC verification failed!\n");
                return FALSE;  // or something other than this
            }
        }
        
	if (!buf.put(m_tmp)) {
		delete m_tmp;
		m_tmp = NULL;
		dprintf(D_ALWAYS, "IO: Packet storing failed\n");
		return FALSE;
	}
		
	if (m_end) {
		ready = TRUE;
	}
	return TRUE;
}


ReliSock::SndMsg::SndMsg() : 
    mode_(MD_OFF), 
    mdChecker_(0),
	p_sock(0),
	m_out_buf(NULL)
{
}

ReliSock::SndMsg::~SndMsg() 
{
    delete mdChecker_;
	delete m_out_buf;
}

void ReliSock::SndMsg::reset()
{
	buf.reset();
	delete m_out_buf;
	m_out_buf = NULL;
}

int ReliSock::SndMsg::finish_packet(const char *peer_description, int sock, int timeout)
{
	if (m_out_buf == NULL) {
		return true;
	}
	dprintf(D_NETWORK, "Finishing packet with non-blocking %d.\n", p_sock->is_non_blocking());
	int retval = true;
	int result = m_out_buf->write(peer_description, sock, -1, timeout, p_sock->is_non_blocking());
	if (result < 0) {
		retval = false;
	} else if (!m_out_buf->consumed()) {
		if (p_sock->is_non_blocking()) {
			return 2;
		} else {
			retval = false;
		}
	}
	delete m_out_buf;
	m_out_buf = NULL;
	return retval;
}

void ReliSock::SndMsg::stash_packet()
{
	dprintf(D_NETWORK, "Stashing packet for later due to non-blocking request.\n");
	m_out_buf = new Buf();
	m_out_buf->swap(buf);
	buf.reset();
}

	// Send the current buffer in a single CEDAR packet.
	// Return codes:
	//   - TRUE: successful send.
	//   - FALSE: failed
	//   - 2: Write would have blocked and we are in non-blocking mode.
	//
	// Note: If we are in non-blocking mode, we may just form the buffer but only
	// partially send it to the wire.  If this happens, we return 2.  In such a case,
	// we leave the SndMsg buffer in a valid state -- if the caller cannot buffer the
	// data itself, it may use the 'put_force' method to grow the underlying buffer.
int ReliSock::SndMsg::snd_packet( char const *peer_description, int _sock, int end, int _timeout )
{
		// First, see if we have an incomplete packet.
	int retval = finish_packet(peer_description, _sock, _timeout);
	if (retval == 2) {
		return 3;
	} else if (!retval) {
		return false;
	}
		// 

	char	        hdr[MAX_HEADER_SIZE];
	int		len, header_size;
	int		ns;

	header_size = (mode_ != MD_OFF) ? MAX_HEADER_SIZE : NORMAL_HEADER_SIZE;
	hdr[0] = (char) end;
	ns = buf.num_used() - header_size;
	len = (int) htonl(ns);
	memcpy(&hdr[1], &len, 4);

	if (!p_sock->get_encryption() && !p_sock->m_finished_send_header && p_sock->_bytes_sent < 1024*1024) {
		if (!p_sock->m_send_md_ctx) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
			p_sock->m_send_md_ctx.reset(EVP_MD_CTX_create());
#else
			p_sock->m_send_md_ctx.reset(EVP_MD_CTX_new());
#endif
			if (!p_sock->m_send_md_ctx) {
				dprintf(D_NETWORK, "IO: Failed to create a new MD context.\n");
				return false;
			}
			if (1 != EVP_DigestInit_ex(p_sock->m_send_md_ctx.get(), EVP_sha256(), NULL)) {
				dprintf(D_NETWORK, "IO: Failed to initialize SHA-256 context.\n");
				return false;
			}
		}
		if (1 != EVP_DigestUpdate(p_sock->m_send_md_ctx.get(), hdr, header_size)) {
			dprintf(D_NETWORK, "IO: Failed to update the message digest.\n");
			return false;
		}
		char hex[3*5 + 1];
		dprintf(D_NETWORK, "Send Header contents: %s\n",
			debug_hex_dump(hex, reinterpret_cast<char*>(hdr), header_size));

		if (1 != EVP_DigestUpdate(p_sock->m_send_md_ctx.get(), buf.get_ptr(), buf.num_untouched())) {
			dprintf(D_NETWORK, "IO: Failed to update the message digest.\n");
			return false;
		}
		dprintf(D_NETWORK, "AESGCM: Send digest added %u + %d bytes \n", header_size, buf.num_untouched());
	}

		// AES-GCM mode encrypts the whole message at send() time; do this now and
		// compute the final digests
	if (p_sock->get_encryption() && p_sock->get_crypto_state()->m_keyInfo.getProtocol() == CONDOR_AESGCM) {

		auto cipher_sz = p_sock->ciphertext_size(buf.num_untouched());
		ns = cipher_sz;
		len = (int) htonl(ns);

		// TODO In a large message, this code will grow the max packet
		//   size by 16 bytes per packet sent. When a small packet
		//   comes through (at the end of a message), the max packet
		//   size will shrink back down (but no smaller than
		//   CONDOR_IO_BUF_SIZE).
		Buf new_buf(p_sock);
		new_buf.grow_buf(cipher_sz + header_size);
		new_buf.alloc_buf();
		memcpy(&hdr[1], &len, 4);
		unsigned char *aad_data = reinterpret_cast<unsigned char *>(hdr);
		int aad_len = header_size;
		std::vector<unsigned char> aad_with_digest;
		if (!p_sock->m_finished_send_header) {
			p_sock->m_finished_send_header = true;
			unsigned md_size = EVP_MD_size(EVP_sha256());
			aad_len = 2*md_size + header_size;
			aad_with_digest.resize(aad_len,0);
			aad_data = &aad_with_digest[0];
			if (!p_sock->m_final_send_header) {
				if (p_sock->m_send_md_ctx &&
					(1 != EVP_DigestFinal_ex(p_sock->m_send_md_ctx.get(), aad_data, &md_size)))
				{
					dprintf(D_NETWORK, "IO: Failed to compute final message digest.\n");
					return false;
				} else if (!p_sock->m_send_md_ctx) {
					memset(aad_data, '\0', md_size);
					dprintf(D_NETWORK|D_VERBOSE, "Setting first digest in AAD to %u 0's\n", md_size);
				} else {
					dprintf(D_NETWORK|D_VERBOSE, "Successfully set first digest in AAD\n");
				}
				p_sock->m_final_send_header = true;
				p_sock->m_final_mds.resize(2*md_size,0);
				memcpy(&p_sock->m_final_mds[0], aad_data, md_size);
			} else {
				memcpy(aad_data, &p_sock->m_final_mds[0], md_size);
			}
			if (!p_sock->m_final_recv_header) {
				if (p_sock->m_recv_md_ctx.get() &&
						(1 != EVP_DigestFinal_ex(p_sock->m_recv_md_ctx.get(), aad_data + md_size, &md_size)))
				{
					dprintf(D_NETWORK, "IO: Failed to compute final receive message digest.\n");
					return false;
				} else if (!p_sock->m_recv_md_ctx) {
					memset(aad_data + md_size, '\0', md_size);
					dprintf(D_NETWORK|D_VERBOSE, "Setting second digest in AAD to %u 0's\n", md_size);
				} else {
					dprintf(D_NETWORK|D_VERBOSE, "Successfully set second digest in AAD when sending\n");
				}
				p_sock->m_final_recv_header = true;
				p_sock->m_final_mds.resize(2*md_size,0);
				memcpy(&p_sock->m_final_mds[0] + md_size, aad_data + md_size, md_size);
			} else {
				memcpy(aad_data + md_size, &p_sock->m_final_mds[0] + md_size, md_size);
			}
			memcpy(aad_data + 2*md_size, hdr, header_size);
			char hex[3*(32*2 + 5) + 1];
			dprintf(D_NETWORK, "Sending AAD with handshake digest %s\n",
				debug_hex_dump(hex, reinterpret_cast<char*>(aad_data), 32*2 + 5));
		}

		if ( ! ((Condor_Crypt_AESGCM*)p_sock->get_crypto())->encrypt(
                        p_sock->crypto_state_,
			aad_data,
			aad_len,
                        static_cast<unsigned char *>(buf.get_ptr()),
			buf.num_untouched(),
			static_cast<unsigned char *>(new_buf.get_ptr()) + header_size,
			cipher_sz))
		{
			dprintf(D_SECURITY, "IO: Failed to encrypt packet\n");
			return false;
		}
		buf.swap(new_buf);
		buf.truncate(cipher_sz + header_size);
	}

		// For non-AES-GCM encryption or unexpectedly large headers, release the memory...
	if (p_sock->m_send_md_ctx && ((p_sock->get_encryption() && p_sock->get_crypto_state()->m_keyInfo.getProtocol() != CONDOR_AESGCM) ||
		(p_sock->m_finished_recv_header && p_sock->m_finished_send_header) || p_sock->_bytes_sent > 1024*1024)) {
		p_sock->m_finished_send_header = true;
		p_sock->m_send_md_ctx.reset();
		dprintf(D_NETWORK, "Resetting Header for send.\n");
	}

	if (mode_ != MD_OFF) {
		if (!buf.computeMD(&hdr[5], mdChecker_)) {
			dprintf(D_ALWAYS, "IO: Failed to compute Message Digest/MAC\n");
			return FALSE;
		}
	}

	int result = buf.flush(peer_description, _sock, hdr, header_size, _timeout, p_sock->is_non_blocking());
	if (result < 0) {
		return false;
	} else if (result != ns+header_size) {
		if (p_sock->is_non_blocking()) {
			stash_packet();
			return 2;
		} else {
			return false;
		}
	}
        
	if( end ) {
		buf.dealloc_buf(); // save space, now that we are done sending
	}
	return TRUE;
}

bool ReliSock::SndMsg::init_MD(CONDOR_MD_MODE mode, KeyInfo * key)
{
    if (!buf.empty()) {
        return false;
    }

    mode_ = mode;
    delete mdChecker_;
	mdChecker_ = 0;

    // only instantiate if we have a key and will use it.  unlike encryption,
    // which previously could be turned on/off during a stream, we either turn
    // MD on at the setup of a stream and leave it on, or we never turn it on
    // at all.
    if (key && (mode != MD_OFF)) {
        mdChecker_ = new Condor_MD_MAC(key);
    }

    return true;
}

#ifndef WIN32
	// interface no longer supported
int 
ReliSock::attach_to_file_desc( int fd )
{
	if (_state != sock_virgin) {
		return FALSE;
	}

	_sock = fd;
	_state = sock_connect;

	int accepting = 0;
	socklen_t l = sizeof(accepting);

	if ((getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &l) == 0) && (l == sizeof(accepting)))
	{
		if (accepting == 1)
		{
			_state = sock_special;
			_special_state = relisock_listen;
		}
	}

	timeout(0);	// make certain in blocking mode
	return TRUE;
}
#endif

Stream::stream_type 
ReliSock::type() const
{ 
	return Stream::reli_sock; 
}

char * ReliSock::serializeMsgInfo() const
{
	char *buf = new char[20 + 3*m_final_mds.size()];
	sprintf(buf, "%i*%i*%i*%i*%zu",
		m_final_send_header,
		m_final_recv_header,
		m_finished_send_header,
		m_finished_recv_header,
		m_final_mds.size()
		);

	if(m_final_mds.size()) {
		strcat(buf, "*");
		char * ptr = buf + strlen(buf);
		const unsigned char * vecdata = m_final_mds.data();
		for (unsigned int i=0; i < m_final_mds.size(); i++, vecdata++, ptr+=2) {
			sprintf(ptr, "%02X", *vecdata);
		}
	}

	dprintf(D_NETWORK|D_VERBOSE, "SERIALIZE: MsgInfo out: %s.\n", buf);

	return buf;
}


const char * ReliSock::serializeMsgInfo(const char * buf)
{
	dprintf(D_NETWORK|D_VERBOSE, "SERIALIZE: reading MsgInfo at beginning of %s.\n", buf);

	size_t vecsize;
	int final_send_header, final_recv_header, finished_send_header, finished_recv_header;

	int num_read = sscanf(buf, "%i*%i*%i*%i*%zu*",
		&final_send_header,
		&final_recv_header,
		&finished_send_header,
		&finished_recv_header,
		&vecsize
		);

	ASSERT(num_read == 5)

	this->m_final_send_header = (final_send_header == 0)       ? false : true;
	this->m_final_recv_header = (final_recv_header == 0)       ? false : true;
	this->m_finished_send_header = (finished_send_header == 0) ? false : true;
	this->m_finished_recv_header = (finished_recv_header == 0) ? false : true;

	dprintf(D_NETWORK|D_VERBOSE, "SERIALIZE: set header vals: %i %i %i %i.\n",
		m_final_send_header,
		m_final_recv_header,
		m_finished_send_header,
		m_finished_recv_header
		);

	// skip to 5th *
	for(int i = 0; i < 5; i++) {
		buf = strchr(buf, '*') + 1;
	}
	buf--;

	dprintf(D_NETWORK|D_VERBOSE, "SERIALIZE: consuming %zu hex bytes of vector data from  %s.\n", vecsize, buf);
	m_final_mds.resize(vecsize);

	int citems = 1;
	if (vecsize) {
		buf++;
		unsigned int hex;
		unsigned char* ptr = m_final_mds.data();
		for (unsigned int i = 0; i < vecsize; i++) {
			citems = sscanf(buf, "%2X", &hex);
			if (citems != 1) break;
			*ptr = (unsigned char)hex;
			buf += 2;  // since we just consumed 2 bytes of hex
			ptr++;      // since we just stored a single byte of binary
		}

	}
	// "EOM" check
	buf = strchr(buf, '*');
	ASSERT( buf && citems == 1 );
	buf++;

	return buf;
}


char *
ReliSock::serialize() const
{
	MyString state;

	char * parent_state = Sock::serialize();
	char * crypto = serializeCryptoInfo();
	char * msg = serializeMsgInfo();
	char * md = serializeMdInfo();

	formatstr( state, "%s%d*%s*%s*%s*%s*", parent_state, _special_state, _who.to_sinful().c_str(), crypto, msg, md );

	delete[] parent_state;
	delete[] crypto;
	delete[] msg;
	delete[] md;

	return state.detach_buffer();
}

const char *
ReliSock::serialize(const char *buf)
{
	char * sinful_string = NULL;
	char fqu[256];
	const char *ptmp, * ptr = NULL;
	int len = 0;

    ASSERT(buf);

	// first, let our parent class restore its state
    ptmp = Sock::serialize(buf);
    ASSERT( ptmp );
    int itmp;
    int citems = sscanf(ptmp,"%d*",&itmp);
	if (citems == 1)
       _special_state = relisock_state(itmp);
    // skip through this
    ptmp = strchr(ptmp, '*');
    if(ptmp) ptmp++;
    // Now, see if we are 6.3 or 6.2
    if (ptmp && (ptr = strchr(ptmp, '*')) != NULL) {
        // we are 6.3
		sinful_string = new char [1 + ptr - ptmp];
        memcpy(sinful_string, ptmp, ptr - ptmp);
		sinful_string[ptr - ptmp] = 0;

        ptmp = ++ptr;
        // The next part is for crypto
        ptmp = serializeCryptoInfo(ptmp);
        // The next part is for message digest state
        ptmp = serializeMsgInfo(ptmp);
        // Followed by Md
        ptmp = serializeMdInfo(ptmp);

        citems = sscanf(ptmp, "%d*", &len);

        if (1 == citems && len > 0 && (ptmp = strchr(ptmp, '*'))) {
            ptmp++;
            memcpy(fqu, ptmp, len);
            if ((fqu[0] != ' ') && (fqu[0] != '\0')) {
                    // We are cozy
				setFullyQualifiedUser(fqu);
            }
        }
    }
    else if(ptmp) {
        // we are 6.2, this is the end of it.
		size_t sinful_len = strlen(ptmp);
		sinful_string = new char [1 + sinful_len];
        citems = sscanf(ptmp,"%s",sinful_string);
		if (1 != citems) sinful_string[0] = 0;
		sinful_string[sinful_len] = 0;
    }

	_who.from_sinful(sinful_string);
	delete [] sinful_string;
    
    return NULL;
}

int 
ReliSock::prepare_for_nobuffering(stream_coding direction)
{
	int ret_val = TRUE;

	if ( direction == stream_unknown ) {
		direction = _coding;
	}

	switch(direction){
		case stream_encode:
			if ( ignore_next_encode_eom == TRUE ) {
				// optimization: if we already prepared for nobuffering,
				// just return true.
				return TRUE;
			}
			if (!snd_msg.buf.empty()) {
				bool is_non_blocking = m_non_blocking;
				m_non_blocking = false;
				ret_val = snd_msg.snd_packet(peer_description(), _sock, TRUE, _timeout);
				m_non_blocking = is_non_blocking;
			}
			if ( ret_val ) {
				ignore_next_encode_eom = TRUE;
			}
			break;

		case stream_decode:
			if ( ignore_next_decode_eom == TRUE ) {
				// optimization: if we already prepared for nobuffering,
				// just return true.
				return TRUE;
			}
			if ( rcv_msg.ready ) {
				if ( !rcv_msg.buf.consumed() )
					ret_val = FALSE;
				rcv_msg.ready = FALSE;
				rcv_msg.buf.reset();
			}
			if ( ret_val ) {
				ignore_next_decode_eom = TRUE;
			}
			break;

		default:
			ASSERT(0);
	}

	return ret_val;
}

int ReliSock::perform_authenticate(bool with_key, KeyInfo *& key, 
								   const char* methods, CondorError* errstack,
								   int auth_timeout, bool non_blocking, char **method_used)
{
	int in_encode_mode;
	int result;

	if( method_used ) {
		*method_used = NULL;
	}

    if (!triedAuthentication()) {
		if (m_authob) {delete m_authob;}
		m_authob = new Authentication(this);
		setTriedAuthentication(true);
			// store if we are in encode or decode mode
		in_encode_mode = is_encode();

			// actually perform the authentication
		if ( with_key ) {
			result = m_authob->authenticate( hostAddr, key, methods, errstack, auth_timeout, non_blocking );
		} else {
			result = m_authob->authenticate( hostAddr, methods, errstack, auth_timeout, non_blocking );
		}
		_should_try_token_request = m_authob->shouldTryTokenRequest();

		if ( result == 2 ) {
			m_auth_in_progress = true;
		}
			// restore stream mode (either encode or decode)
		if ( in_encode_mode && is_decode() ) {
			encode();
		} else {
			if ( !in_encode_mode && is_encode() ) { 
				decode();
			}
		}

		if (!m_auth_in_progress) {
			int result2 = authenticate_continue(errstack, non_blocking, method_used);
			return result ? result2 : 0;
		}
		return result;
    }
    else {
        return 1;
    }
}

int ReliSock::authenticate_continue(CondorError* errstack, bool non_blocking, char **method_used)
{
	int result = 1;
	if( m_auth_in_progress )
	{
		result = m_authob->authenticate_continue(errstack, non_blocking);
		_should_try_token_request = m_authob->shouldTryTokenRequest();
		if (result == 2) {
			return result;
		}
	}
	m_auth_in_progress = false;

	setFullyQualifiedUser(m_authob->getFullyQualifiedUser());

	if( m_authob->getMethodUsed() ) {
		setAuthenticationMethodUsed(m_authob->getMethodUsed());
		if( method_used ) {
			*method_used = strdup(m_authob->getMethodUsed());
		}
	}
	if ( m_authob->getFQAuthenticatedName() ) {
		setAuthenticatedName( m_authob->getFQAuthenticatedName() );
	}
	delete m_authob;
	m_authob = NULL;
	return result;
}

int ReliSock::authenticate(KeyInfo *& key, const char* methods, CondorError* errstack, int auth_timeout, bool non_blocking, char **method_used)
{
	return perform_authenticate(true,key,methods,errstack,auth_timeout,non_blocking,method_used);
}

int 
ReliSock::authenticate(const char* methods, CondorError* errstack, int auth_timeout, bool non_blocking) 
{
	KeyInfo *key = NULL;
	return perform_authenticate(false,key,methods,errstack,auth_timeout,non_blocking,NULL);
}

bool
ReliSock::connect_socketpair_impl( ReliSock & sock, condor_protocol proto, bool isLoopback ) {
	ReliSock tmp;
	if( ! tmp.bind( proto, false, 0, isLoopback ) ) {
		dprintf( D_ALWAYS, "connect_socketpair(): failed to bind() that.\n" );
		return false;
	}

	if( !tmp.listen() ) {
		dprintf( D_ALWAYS, "connect_socketpair(): failed to listen() on that.\n" );
		return false;
	}

	if( ! bind( proto, false, 0, isLoopback ) ) {
		dprintf( D_ALWAYS, "connect_socketpair(): failed to bind() this.\n" );
		return false;
	}

	if( !connect( tmp.my_ip_str(), tmp.get_port() ) ) {
		dprintf( D_ALWAYS, "connect_socketpair(): failed to connect() to that.\n" );
		return false;
	}

	tmp.timeout( 1 );
	if( ! tmp.accept( sock ) ) {
		dprintf( D_ALWAYS, "connect_socketpair(): failed to accept() that.\n" );
		return false;
	}

	return true;
}

bool
ReliSock::connect_socketpair( ReliSock & sock, char const * asIfConnectingTo ) {
	condor_sockaddr aictAddr;
	if( ! aictAddr.from_ip_string( asIfConnectingTo ) ) {
		dprintf( D_ALWAYS, "connect_socketpair(): '%s' not a valid IP string.\n", asIfConnectingTo );
		return false;
	}

	return connect_socketpair_impl( sock, aictAddr.get_protocol(), aictAddr.is_loopback() );
}

bool
ReliSock::connect_socketpair( ReliSock & sock ) {
	condor_protocol proto = CP_IPV4;
	bool ipV4Allowed = ! param_false( "ENABLE_IPV4" );
	bool ipV6Allowed = ! param_false( "ENABLE_IPV6" );

	if( ipV6Allowed && (! ipV4Allowed) ) {
		proto = CP_IPV6;
	}

	return connect_socketpair_impl( sock, proto, true );
}

void
ReliSock::enter_reverse_connecting_state()
{
	if( _state == sock_assigned ) {
		// no need for a socket to be allocated while we are waiting
		// because this socket will be assigned to a new socket
		// once we accept a connection from the listen socket
		this->close();
	}
	ASSERT( _state == sock_virgin );
	_state = sock_reverse_connect_pending;
}

void
ReliSock::exit_reverse_connecting_state(ReliSock *sock)
{
	ASSERT( _state == sock_reverse_connect_pending );
	_state = sock_virgin;

	if( sock ) {
		int assign_rc = assignCCBSocket(sock->get_file_desc());
		ASSERT( assign_rc );
		isClient(true);
		if( sock->_state == sock_connect ) {
			enter_connected_state("REVERSE CONNECT");
		}
		else {
			_state = sock->_state;
		}
		sock->_sock = INVALID_SOCKET;
		sock->close();
	}
	m_ccb_client = NULL;
}

/*
 * Use the Linux-specific TCP_INFO getsockopt to return a human readable
 * string describing low-level tcp statistics
 *
 */
char *
ReliSock::get_statistics() {
#ifndef LINUX
	return 0;
#else
			// 20 entries, each with 16 bytes of text and up to 10 bytes numeric
	int maxSize = 20 * (16 + 10);
	if (statsBuf == 0) {
		statsBuf = (char *)malloc(maxSize + 1); // dtor frees this
		statsBuf[0] = '\0';
	}

	struct tcp_info ti;
	socklen_t tcp_info_len = sizeof(struct tcp_info);

	int ret = getsockopt(this->get_file_desc(), SOL_TCP, TCP_INFO, &ti, &tcp_info_len);
	if (ret != 0) {
			// maybe the sock was closed?  Return the last value, if any.
		return statsBuf;
	}

#if __GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ > 6)
	snprintf(statsBuf, maxSize,
		"rto: %d "
		"ato: %d "
		"snd_mss: %d "
		"rcv_mss: %d "
		"unacked: %d "
		"sacked: %d "
		"lost: %d "
		"retrans: %d "
		"fackets: %d "
		"pmtu: %d "
		"rcv_ssthresh: %d "
		"rtt: %d "
		"snd_ssthresh: %d "
		"snd_cwnd: %d "
		"advmss: %d "
		"reordering: %d "
		"rcv_rtt: %d "
		"rcv_space: %d "
		"total_retrans: %d ",
		
		ti.tcpi_rto,
		ti.tcpi_ato,
		ti.tcpi_snd_mss,
		ti.tcpi_rcv_mss,
		ti.tcpi_unacked,
		ti.tcpi_sacked,
		ti.tcpi_lost,
		ti.tcpi_retrans,
		ti.tcpi_fackets,
		ti.tcpi_pmtu,
		ti.tcpi_rcv_ssthresh,
		ti.tcpi_rtt,
		ti.tcpi_snd_ssthresh,
		ti.tcpi_snd_cwnd,
		ti.tcpi_advmss,
		ti.tcpi_reordering,
		ti.tcpi_rcv_rtt,
		ti.tcpi_rcv_space,
		ti.tcpi_total_retrans);
#endif
		
	return statsBuf;
#endif
}

void
ReliSock::setTargetSharedPortID( char const *id )
{
	if( m_target_shared_port_id ) {
		free( m_target_shared_port_id );
		m_target_shared_port_id  = NULL;
	}
	if( id ) {
		m_target_shared_port_id = strdup( id );
	}
}

bool
ReliSock::msgReady() {
	while (!rcv_msg.ready)
	{
		// NOTE: 'true' here indicates non-blocking.
		BlockingModeGuard sentry(this, true);
		int retval = handle_incoming_packet();
		if (retval == 2) {
			dprintf(D_NETWORK, "msgReady would have blocked.\n");
			m_read_would_block = true;
			return false;
		} else if (retval == 0) {
			// No data is available
			return false;
		}
	}
	return rcv_msg.ready;
}
