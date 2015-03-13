#include "tls_wrap.h"
#include "async-wrap.h"
#include "async-wrap-inl.h"
#include "node_buffer.h"  // Buffer
#include "node_crypto.h"  // SecureContext
#include "node_crypto_bio.h"  // NodeBIO
#include "node_crypto_clienthello.h"  // ClientHelloParser
#include "node_crypto_clienthello-inl.h"
#include "node_wrap.h"  // WithGenericStream
#include "node_counters.h"
#include "node_internals.h"
#include "util.h"
#include "util-inl.h"

namespace node {

using crypto::SSLWrap;
using crypto::SecureContext;
using v8::Boolean;
using v8::Context;
using v8::EscapableHandleScope;
using v8::Exception;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Handle;
using v8::HandleScope;
using v8::Integer;
using v8::Local;
using v8::Null;
using v8::Object;
using v8::String;
using v8::Value;


TLSWrap::TLSWrap(Environment* env,
                 Kind kind,
                 StreamBase* stream,
                 Handle<Object> stream_obj,
                 Handle<Object> sc)
    : SSLWrap<TLSWrap>(env, Unwrap<SecureContext>(sc), kind),
      StreamBase(env),
      AsyncWrap(env,
                env->tls_wrap_constructor_function()->NewInstance(),
                AsyncWrap::PROVIDER_TLSWRAP),
      sc_(Unwrap<SecureContext>(sc)),
      sc_handle_(env->isolate(), sc),
      stream_(stream),
      stream_handle_(env->isolate(), stream_obj),
      enc_in_(nullptr),
      enc_out_(nullptr),
      clear_in_(nullptr),
      write_size_(0),
      started_(false),
      established_(false),
      shutdown_(false),
      error_(nullptr),
      cycle_depth_(0),
      eof_(false) {
  node::Wrap(object(), this);
  MakeWeak(this);

  // We've our own session callbacks
  SSL_CTX_sess_set_get_cb(sc_->ctx_, SSLWrap<TLSWrap>::GetSessionCallback);
  SSL_CTX_sess_set_new_cb(sc_->ctx_, SSLWrap<TLSWrap>::NewSessionCallback);

  stream_->Consume();
  stream_->set_after_write_cb(OnAfterWriteImpl, this);
  stream_->set_alloc_cb(OnAllocImpl, this);
  stream_->set_read_cb(OnReadImpl, this);

  set_alloc_cb(OnAllocSelf, this);
  set_read_cb(OnReadSelf, this);

  InitSSL();
}


TLSWrap::~TLSWrap() {
  enc_in_ = nullptr;
  enc_out_ = nullptr;
  delete clear_in_;
  clear_in_ = nullptr;

  sc_ = nullptr;
  sc_handle_.Reset();
  stream_handle_.Reset();
  persistent().Reset();

#ifdef SSL_CTRL_SET_TLSEXT_SERVERNAME_CB
  sni_context_.Reset();
#endif  // SSL_CTRL_SET_TLSEXT_SERVERNAME_CB

  // Move all writes to pending
  MakePending();

  // And destroy
  while (WriteItem* wi = pending_write_items_.PopFront())
    delete wi;

  ClearError();
}


void TLSWrap::MakePending() {
  write_item_queue_.MoveBack(&pending_write_items_);
}


bool TLSWrap::InvokeQueued(int status) {
  if (pending_write_items_.IsEmpty())
    return false;

  // Process old queue
  WriteItemList queue;
  pending_write_items_.MoveBack(&queue);
  while (WriteItem* wi = queue.PopFront()) {
    wi->w_->Done(status);
    delete wi;
  }

  return true;
}


void TLSWrap::NewSessionDoneCb() {
  Cycle();
}


void TLSWrap::InitSSL() {
  // Initialize SSL
  enc_in_ = NodeBIO::New();
  enc_out_ = NodeBIO::New();
  NodeBIO::FromBIO(enc_in_)->AssignEnvironment(env());
  NodeBIO::FromBIO(enc_out_)->AssignEnvironment(env());

  SSL_set_bio(ssl_, enc_in_, enc_out_);

  // NOTE: This could be overriden in SetVerifyMode
  SSL_set_verify(ssl_, SSL_VERIFY_NONE, crypto::VerifyCallback);

#ifdef SSL_MODE_RELEASE_BUFFERS
  long mode = SSL_get_mode(ssl_);
  SSL_set_mode(ssl_, mode | SSL_MODE_RELEASE_BUFFERS);
#endif  // SSL_MODE_RELEASE_BUFFERS

  SSL_set_app_data(ssl_, this);
  SSL_set_info_callback(ssl_, SSLInfoCallback);

#ifdef SSL_CTRL_SET_TLSEXT_SERVERNAME_CB
  if (is_server()) {
    SSL_CTX_set_tlsext_servername_callback(sc_->ctx_, SelectSNIContextCallback);
  }
#endif  // SSL_CTRL_SET_TLSEXT_SERVERNAME_CB

  InitNPN(sc_);

  if (is_server()) {
    SSL_set_accept_state(ssl_);
  } else if (is_client()) {
    // Enough space for server response (hello, cert)
    NodeBIO::FromBIO(enc_in_)->set_initial(kInitialClientBufferLength);
    SSL_set_connect_state(ssl_);
  } else {
    // Unexpected
    abort();
  }

  // Initialize ring for queud clear data
  clear_in_ = new NodeBIO();
  clear_in_->AssignEnvironment(env());
}


void TLSWrap::Wrap(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  if (args.Length() < 1 || !args[0]->IsObject()) {
    return env->ThrowTypeError(
        "First argument should be a StreamWrap instance");
  }
  if (args.Length() < 2 || !args[1]->IsObject()) {
    return env->ThrowTypeError(
        "Second argument should be a SecureContext instance");
  }
  if (args.Length() < 3 || !args[2]->IsBoolean())
    return env->ThrowTypeError("Third argument should be boolean");

  Local<Object> stream_obj = args[0].As<Object>();
  Local<Object> sc = args[1].As<Object>();
  Kind kind = args[2]->IsTrue() ? SSLWrap<TLSWrap>::kServer :
                                  SSLWrap<TLSWrap>::kClient;

  StreamBase* stream = nullptr;
  WITH_GENERIC_STREAM(env, stream_obj, {
    stream = wrap;
  });
  CHECK_NE(stream, nullptr);

  TLSWrap* res = new TLSWrap(env, kind, stream, stream_obj, sc);

  args.GetReturnValue().Set(res->persistent());
}


void TLSWrap::Receive(const FunctionCallbackInfo<Value>& args) {
  TLSWrap* wrap = Unwrap<TLSWrap>(args.Holder());

  CHECK(Buffer::HasInstance(args[0]));
  char* data = Buffer::Data(args[0]);
  size_t len = Buffer::Length(args[0]);

  uv_buf_t buf;

  // Copy given buffer entirely or partiall if handle becomes closed
  while (len > 0 && !wrap->IsClosing()) {
    wrap->stream_->OnAlloc(len, &buf);
    size_t copy = buf.len > len ? len : buf.len;
    memcpy(buf.base, data, copy);
    buf.len = copy;
    wrap->stream_->OnRead(buf.len, &buf);

    data += copy;
    len -= copy;
  }
}


void TLSWrap::Start(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  TLSWrap* wrap = Unwrap<TLSWrap>(args.Holder());

  if (wrap->started_)
    return env->ThrowError("Already started.");
  wrap->started_ = true;

  // Send ClientHello handshake
  CHECK(wrap->is_client());
  wrap->ClearOut();
  wrap->EncOut();
}


void TLSWrap::SSLInfoCallback(const SSL* ssl_, int where, int ret) {
  if (!(where & (SSL_CB_HANDSHAKE_START | SSL_CB_HANDSHAKE_DONE)))
    return;

  // Be compatible with older versions of OpenSSL. SSL_get_app_data() wants
  // a non-const SSL* in OpenSSL <= 0.9.7e.
  SSL* ssl = const_cast<SSL*>(ssl_);
  TLSWrap* c = static_cast<TLSWrap*>(SSL_get_app_data(ssl));
  Environment* env = c->env();
  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());
  Local<Object> object = c->object();

  if (where & SSL_CB_HANDSHAKE_START) {
    Local<Value> callback = object->Get(env->onhandshakestart_string());
    if (callback->IsFunction()) {
      c->MakeCallback(callback.As<Function>(), 0, nullptr);
    }
  }

  if (where & SSL_CB_HANDSHAKE_DONE) {
    c->established_ = true;
    Local<Value> callback = object->Get(env->onhandshakedone_string());
    if (callback->IsFunction()) {
      c->MakeCallback(callback.As<Function>(), 0, nullptr);
    }
  }
}


void TLSWrap::EncOut() {
  // Ignore cycling data if ClientHello wasn't yet parsed
  if (!hello_parser_.IsEnded())
    return;

  // Write in progress
  if (write_size_ != 0)
    return;

  // Wait for `newSession` callback to be invoked
  if (is_waiting_new_session())
    return;

  // Split-off queue
  if (established_ && !write_item_queue_.IsEmpty())
    MakePending();

  // No data to write
  if (BIO_pending(enc_out_) == 0) {
    if (clear_in_->Length() == 0)
      InvokeQueued(0);
    return;
  }

  char* data[kSimultaneousBufferCount];
  size_t size[ARRAY_SIZE(data)];
  size_t count = ARRAY_SIZE(data);
  write_size_ = NodeBIO::FromBIO(enc_out_)->PeekMultiple(data, size, &count);
  CHECK(write_size_ != 0 && count != 0);

  Local<Object> req_wrap_obj =
      env()->write_wrap_constructor_function()->NewInstance();
  char* storage = new char[sizeof(WriteWrap)];
  WriteWrap* write_req = new(storage) WriteWrap(env(),
                                                req_wrap_obj,
                                                this,
                                                EncOutCb);

  uv_buf_t buf[ARRAY_SIZE(data)];
  for (size_t i = 0; i < count; i++)
    buf[i] = uv_buf_init(data[i], size[i]);
  int r = stream_->DoWrite(write_req, buf, count, nullptr);

  // Ignore errors, this should be already handled in js
  if (!r)
    NODE_COUNT_NET_BYTES_SENT(write_size_);
}


void TLSWrap::EncOutCb(WriteWrap* req_wrap, int status) {
  TLSWrap* wrap = req_wrap->wrap()->Cast<TLSWrap>();

  // Handle error
  if (status) {
    // Ignore errors after shutdown
    if (wrap->shutdown_)
      return;

    // Notify about error
    wrap->InvokeQueued(status);
    return;
  }

  // Commit
  NodeBIO::FromBIO(wrap->enc_out_)->Read(nullptr, wrap->write_size_);

  // Try writing more data
  wrap->write_size_ = 0;
  wrap->EncOut();
}


Local<Value> TLSWrap::GetSSLError(int status, int* err, const char** msg) {
  EscapableHandleScope scope(env()->isolate());

  *err = SSL_get_error(ssl_, status);
  switch (*err) {
    case SSL_ERROR_NONE:
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
      break;
    case SSL_ERROR_ZERO_RETURN:
      return scope.Escape(env()->zero_return_string());
      break;
    default:
      {
        CHECK(*err == SSL_ERROR_SSL || *err == SSL_ERROR_SYSCALL);

        BIO* bio = BIO_new(BIO_s_mem());
        ERR_print_errors(bio);

        BUF_MEM* mem;
        BIO_get_mem_ptr(bio, &mem);

        Local<String> message =
            OneByteString(env()->isolate(), mem->data, mem->length);
        Local<Value> exception = Exception::Error(message);

        if (msg != nullptr) {
          CHECK_EQ(*msg, nullptr);
          char* const buf = new char[mem->length + 1];
          memcpy(buf, mem->data, mem->length);
          buf[mem->length] = '\0';
          *msg = buf;
        }
        static_cast<void>(BIO_reset(bio));

        return scope.Escape(exception);
      }
  }
  return Local<Value>();
}


void TLSWrap::ClearOut() {
  // Ignore cycling data if ClientHello wasn't yet parsed
  if (!hello_parser_.IsEnded())
    return;

  // No reads after EOF
  if (eof_)
    return;

  HandleScope handle_scope(env()->isolate());
  Context::Scope context_scope(env()->context());

  CHECK_NE(ssl_, nullptr);

  char out[kClearOutChunkSize];
  int read;
  for (;;) {
    read = SSL_read(ssl_, out, sizeof(out));

    if (read <= 0)
      break;

    while (read > 0) {
      int avail = read;

      uv_buf_t buf;
      OnAlloc(avail, &buf);
      if (static_cast<int>(buf.len) < avail)
        avail = buf.len;
      memcpy(buf.base, out, avail);
      OnRead(avail, &buf);

      read -= avail;
    }
  }

  int flags = SSL_get_shutdown(ssl_);
  if (!eof_ && flags & SSL_RECEIVED_SHUTDOWN) {
    eof_ = true;
    OnRead(UV_EOF, nullptr);
  }

  if (read == -1) {
    int err;
    Local<Value> arg = GetSSLError(read, &err, nullptr);

    // Ignore ZERO_RETURN after EOF, it is basically not a error
    if (err == SSL_ERROR_ZERO_RETURN && eof_)
      return;

    if (!arg.IsEmpty()) {
      // When TLS Alert are stored in wbio,
      // it should be flushed to socket before destroyed.
      if (BIO_pending(enc_out_) != 0)
        EncOut();

      MakeCallback(env()->onerror_string(), 1, &arg);
    }
  }
}


bool TLSWrap::ClearIn() {
  // Ignore cycling data if ClientHello wasn't yet parsed
  if (!hello_parser_.IsEnded())
    return false;

  int written = 0;
  while (clear_in_->Length() > 0) {
    size_t avail = 0;
    char* data = clear_in_->Peek(&avail);
    written = SSL_write(ssl_, data, avail);
    CHECK(written == -1 || written == static_cast<int>(avail));
    if (written == -1)
      break;
    clear_in_->Read(nullptr, avail);
  }

  // All written
  if (clear_in_->Length() == 0) {
    CHECK_GE(written, 0);
    return true;
  }

  HandleScope handle_scope(env()->isolate());
  Context::Scope context_scope(env()->context());

  // Error or partial write
  int err;
  Local<Value> arg = GetSSLError(written, &err, &error_);
  if (!arg.IsEmpty()) {
    MakePending();
    if (!InvokeQueued(UV_EPROTO))
      ClearError();
    clear_in_->Reset();
  }

  return false;
}


void* TLSWrap::Cast() {
  return reinterpret_cast<void*>(this);
}


AsyncWrap* TLSWrap::GetAsyncWrap() {
  return static_cast<AsyncWrap*>(this);
}


bool TLSWrap::IsIPCPipe() {
  return stream_->IsIPCPipe();
}


int TLSWrap::GetFD() {
  return stream_->GetFD();
}


bool TLSWrap::IsAlive() {
  return stream_->IsAlive();
}


bool TLSWrap::IsClosing() {
  return stream_->IsClosing();
}


int TLSWrap::ReadStart() {
  return stream_->ReadStart();
}


int TLSWrap::ReadStop() {
  return stream_->ReadStop();
}


const char* TLSWrap::Error() const {
  return error_;
}


void TLSWrap::ClearError() {
  delete[] error_;
  error_ = nullptr;
}


int TLSWrap::DoWrite(WriteWrap* w,
                     uv_buf_t* bufs,
                     size_t count,
                     uv_stream_t* send_handle) {
  CHECK_EQ(send_handle, nullptr);

  bool empty = true;

  // Empty writes should not go through encryption process
  size_t i;
  for (i = 0; i < count; i++)
    if (bufs[i].len > 0) {
      empty = false;
      break;
    }
  if (empty) {
    ClearOut();
    // However if there any data that should be written to socket,
    // callback should not be invoked immediately
    if (BIO_pending(enc_out_) == 0)
      return stream_->DoWrite(w, bufs, count, send_handle);
  }

  // Queue callback to execute it on next tick
  write_item_queue_.PushBack(new WriteItem(w));

  // Write queued data
  if (empty) {
    EncOut();
    return 0;
  }

  // Process enqueued data first
  if (!ClearIn()) {
    // If there're still data to process - enqueue current one
    for (i = 0; i < count; i++)
      clear_in_->Write(bufs[i].base, bufs[i].len);
    return 0;
  }

  int written = 0;
  for (i = 0; i < count; i++) {
    written = SSL_write(ssl_, bufs[i].base, bufs[i].len);
    CHECK(written == -1 || written == static_cast<int>(bufs[i].len));
    if (written == -1)
      break;
  }

  if (i != count) {
    int err;
    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());
    Local<Value> arg = GetSSLError(written, &err, &error_);
    if (!arg.IsEmpty())
      return UV_EPROTO;

    // No errors, queue rest
    for (; i < count; i++)
      clear_in_->Write(bufs[i].base, bufs[i].len);
  }

  // Try writing data immediately
  EncOut();

  return 0;
}


void TLSWrap::OnAfterWriteImpl(WriteWrap* w, void* ctx) {
  // Intentionally empty
}


void TLSWrap::OnAllocImpl(size_t suggested_size, uv_buf_t* buf, void* ctx) {
  TLSWrap* wrap = static_cast<TLSWrap*>(ctx);

  size_t size = 0;
  buf->base = NodeBIO::FromBIO(wrap->enc_in_)->PeekWritable(&size);
  buf->len = size;
}


void TLSWrap::OnReadImpl(ssize_t nread,
                         const uv_buf_t* buf,
                         uv_handle_type pending,
                         void* ctx) {
  TLSWrap* wrap = static_cast<TLSWrap*>(ctx);
  wrap->DoRead(nread, buf, pending);
}


void TLSWrap::OnAllocSelf(size_t suggested_size, uv_buf_t* buf, void* ctx) {
  buf->base = static_cast<char*>(malloc(suggested_size));
  CHECK_NE(buf->base, nullptr);
  buf->len = suggested_size;
}


void TLSWrap::OnReadSelf(ssize_t nread,
                         const uv_buf_t* buf,
                         uv_handle_type pending,
                         void* ctx) {
  TLSWrap* wrap = static_cast<TLSWrap*>(ctx);
  Local<Object> buf_obj;
  if (buf != nullptr)
    buf_obj = Buffer::Use(wrap->env(), buf->base, buf->len);
  wrap->EmitData(nread, buf_obj, Local<Object>());
}


void TLSWrap::DoRead(ssize_t nread,
                     const uv_buf_t* buf,
                     uv_handle_type pending) {
  if (nread < 0)  {
    // Error should be emitted only after all data was read
    ClearOut();

    // Ignore EOF if received close_notify
    if (nread == UV_EOF) {
      if (eof_)
        return;
      eof_ = true;
    }

    HandleScope handle_scope(env()->isolate());
    Context::Scope context_scope(env()->context());
    OnRead(nread, nullptr);
    return;
  }

  // Only client connections can receive data
  CHECK_NE(ssl_, nullptr);

  // Commit read data
  NodeBIO* enc_in = NodeBIO::FromBIO(enc_in_);
  enc_in->Commit(nread);

  // Parse ClientHello first
  if (!hello_parser_.IsEnded()) {
    size_t avail = 0;
    uint8_t* data = reinterpret_cast<uint8_t*>(enc_in->Peek(&avail));
    CHECK(avail == 0 || data != nullptr);
    return hello_parser_.Parse(data, avail);
  }

  // Cycle OpenSSL's state
  Cycle();
}


int TLSWrap::DoShutdown(ShutdownWrap* req_wrap) {
  if (SSL_shutdown(ssl_) == 0)
    SSL_shutdown(ssl_);
  shutdown_ = true;
  EncOut();
  return stream_->DoShutdown(req_wrap);
}


void TLSWrap::SetVerifyMode(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  TLSWrap* wrap = Unwrap<TLSWrap>(args.Holder());

  if (args.Length() < 2 || !args[0]->IsBoolean() || !args[1]->IsBoolean())
    return env->ThrowTypeError("Bad arguments, expected two booleans");

  int verify_mode;
  if (wrap->is_server()) {
    bool request_cert = args[0]->IsTrue();
    if (!request_cert) {
      // Note reject_unauthorized ignored.
      verify_mode = SSL_VERIFY_NONE;
    } else {
      bool reject_unauthorized = args[1]->IsTrue();
      verify_mode = SSL_VERIFY_PEER;
      if (reject_unauthorized)
        verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    }
  } else {
    // Note request_cert and reject_unauthorized are ignored for clients.
    verify_mode = SSL_VERIFY_NONE;
  }

  // Always allow a connection. We'll reject in javascript.
  SSL_set_verify(wrap->ssl_, verify_mode, crypto::VerifyCallback);
}


void TLSWrap::EnableSessionCallbacks(
    const FunctionCallbackInfo<Value>& args) {
  TLSWrap* wrap = Unwrap<TLSWrap>(args.Holder());
  wrap->enable_session_callbacks();
  EnableHelloParser(args);
}


void TLSWrap::EnableHelloParser(const FunctionCallbackInfo<Value>& args) {
  TLSWrap* wrap = Unwrap<TLSWrap>(args.Holder());
  NodeBIO::FromBIO(wrap->enc_in_)->set_initial(kMaxHelloLength);
  wrap->hello_parser_.Start(SSLWrap<TLSWrap>::OnClientHello,
                            OnClientHelloParseEnd,
                            wrap);
}


void TLSWrap::OnClientHelloParseEnd(void* arg) {
  TLSWrap* c = static_cast<TLSWrap*>(arg);
  c->Cycle();
}


#ifdef SSL_CTRL_SET_TLSEXT_SERVERNAME_CB
void TLSWrap::GetServername(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  TLSWrap* wrap = Unwrap<TLSWrap>(args.Holder());

  const char* servername = SSL_get_servername(wrap->ssl_,
                                              TLSEXT_NAMETYPE_host_name);
  if (servername != nullptr) {
    args.GetReturnValue().Set(OneByteString(env->isolate(), servername));
  } else {
    args.GetReturnValue().Set(false);
  }
}


void TLSWrap::SetServername(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  TLSWrap* wrap = Unwrap<TLSWrap>(args.Holder());

  if (args.Length() < 1 || !args[0]->IsString())
    return env->ThrowTypeError("First argument should be a string");

  if (wrap->started_)
    return env->ThrowError("Already started.");

  if (!wrap->is_client())
    return;

#ifdef SSL_CTRL_SET_TLSEXT_SERVERNAME_CB
  node::Utf8Value servername(env->isolate(), args[0].As<String>());
  SSL_set_tlsext_host_name(wrap->ssl_, *servername);
#endif  // SSL_CTRL_SET_TLSEXT_SERVERNAME_CB
}


int TLSWrap::SelectSNIContextCallback(SSL* s, int* ad, void* arg) {
  TLSWrap* p = static_cast<TLSWrap*>(SSL_get_app_data(s));
  Environment* env = p->env();

  const char* servername = SSL_get_servername(s, TLSEXT_NAMETYPE_host_name);

  if (servername == nullptr)
    return SSL_TLSEXT_ERR_OK;

  HandleScope scope(env->isolate());
  // Call the SNI callback and use its return value as context
  Local<Object> object = p->object();
  Local<Value> ctx = object->Get(env->sni_context_string());

  // Not an object, probably undefined or null
  if (!ctx->IsObject())
    return SSL_TLSEXT_ERR_NOACK;

  Local<FunctionTemplate> cons = env->secure_context_constructor_template();
  if (!cons->HasInstance(ctx)) {
    // Failure: incorrect SNI context object
    Local<Value> err = Exception::TypeError(env->sni_context_err_string());
    p->MakeCallback(env->onerror_string(), 1, &err);
    return SSL_TLSEXT_ERR_NOACK;
  }

  p->sni_context_.Reset();
  p->sni_context_.Reset(env->isolate(), ctx);

  SecureContext* sc = Unwrap<SecureContext>(ctx.As<Object>());
  InitNPN(sc);
  SSL_set_SSL_CTX(s, sc->ctx_);
  return SSL_TLSEXT_ERR_OK;
}
#endif  // SSL_CTRL_SET_TLSEXT_SERVERNAME_CB


void TLSWrap::Initialize(Handle<Object> target,
                              Handle<Value> unused,
                              Handle<Context> context) {
  Environment* env = Environment::GetCurrent(context);

  env->SetMethod(target, "wrap", TLSWrap::Wrap);

  Local<FunctionTemplate> t = FunctionTemplate::New(env->isolate());
  t->InstanceTemplate()->SetInternalFieldCount(1);
  t->SetClassName(FIXED_ONE_BYTE_STRING(env->isolate(), "TLSWrap"));

  env->SetProtoMethod(t, "receive", Receive);
  env->SetProtoMethod(t, "start", Start);
  env->SetProtoMethod(t, "setVerifyMode", SetVerifyMode);
  env->SetProtoMethod(t, "enableSessionCallbacks", EnableSessionCallbacks);
  env->SetProtoMethod(t, "enableHelloParser", EnableHelloParser);

  StreamBase::AddMethods<TLSWrap>(env, t);
  SSLWrap<TLSWrap>::AddMethods(env, t);

#ifdef SSL_CTRL_SET_TLSEXT_SERVERNAME_CB
  env->SetProtoMethod(t, "getServername", GetServername);
  env->SetProtoMethod(t, "setServername", SetServername);
#endif  // SSL_CRT_SET_TLSEXT_SERVERNAME_CB

  env->set_tls_wrap_constructor_template(t);
  env->set_tls_wrap_constructor_function(t->GetFunction());
}

}  // namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(tls_wrap, node::TLSWrap::Initialize)
