#include "base_object-inl.h"
#include "crypto_x509.h"
#include "crypto_common.h"
#include "crypto_context.h"
#include "crypto_keys.h"
#include "crypto_bio.h"
#include "env-inl.h"
#include "memory_tracker-inl.h"
#include "node_errors.h"
#include "util-inl.h"
#include "v8.h"

#include <string>
#include <vector>

namespace node {

using v8::Array;
using v8::ArrayBufferView;
using v8::Context;
using v8::EscapableHandleScope;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Local;
using v8::MaybeLocal;
using v8::Object;
using v8::Uint32;
using v8::Value;

namespace crypto {

ManagedX509::ManagedX509(X509Pointer&& cert) : cert_(std::move(cert)) {}

ManagedX509::ManagedX509(const ManagedX509& that) {
  *this = that;
}

ManagedX509& ManagedX509::operator=(const ManagedX509& that) {
  cert_.reset(that.get());

  if (cert_)
    X509_up_ref(cert_.get());

  return *this;
}

void ManagedX509::MemoryInfo(MemoryTracker* tracker) const {
  // This is an approximation based on the der encoding size.
  int size = i2d_X509(cert_.get(), nullptr);
  tracker->TrackFieldWithSize("cert", size);
}

Local<FunctionTemplate> X509Certificate::GetConstructorTemplate(
    Environment* env) {
  Local<FunctionTemplate> tmpl = env->x509_constructor_template();
  if (tmpl.IsEmpty()) {
    tmpl = FunctionTemplate::New(env->isolate());
    tmpl->InstanceTemplate()->SetInternalFieldCount(1);
    tmpl->Inherit(BaseObject::GetConstructorTemplate(env));
    tmpl->SetClassName(
        FIXED_ONE_BYTE_STRING(env->isolate(), "X509Certificate"));
    env->SetProtoMethod(tmpl, "subject", Subject);
    env->SetProtoMethod(tmpl, "subjectAltName", SubjectAltName);
    env->SetProtoMethod(tmpl, "infoAccess", InfoAccess);
    env->SetProtoMethod(tmpl, "issuer", Issuer);
    env->SetProtoMethod(tmpl, "validTo", ValidTo);
    env->SetProtoMethod(tmpl, "validFrom", ValidFrom);
    env->SetProtoMethod(tmpl, "fingerprint", Fingerprint);
    env->SetProtoMethod(tmpl, "fingerprint256", Fingerprint256);
    env->SetProtoMethod(tmpl, "keyUsage", KeyUsage);
    env->SetProtoMethod(tmpl, "serialNumber", SerialNumber);
    env->SetProtoMethod(tmpl, "pem", Pem);
    env->SetProtoMethod(tmpl, "raw", Raw);
    env->SetProtoMethod(tmpl, "publicKey", PublicKey);
    env->SetProtoMethod(tmpl, "checkCA", CheckCA);
    env->SetProtoMethod(tmpl, "checkHost", CheckHost);
    env->SetProtoMethod(tmpl, "checkEmail", CheckEmail);
    env->SetProtoMethod(tmpl, "checkIP", CheckIP);
    env->SetProtoMethod(tmpl, "checkIssued", CheckIssued);
    env->SetProtoMethod(tmpl, "checkPrivateKey", CheckPrivateKey);
    env->SetProtoMethod(tmpl, "verify", Verify);
    env->SetProtoMethod(tmpl, "toLegacy", ToLegacy);
    env->set_x509_constructor_template(tmpl);
  }
  return tmpl;
}

bool X509Certificate::HasInstance(Environment* env, Local<Object> object) {
  return GetConstructorTemplate(env)->HasInstance(object);
}

MaybeLocal<Object> X509Certificate::New(
    Environment* env,
    X509Pointer cert) {
  std::shared_ptr<ManagedX509> mcert(new ManagedX509(std::move(cert)));
  return New(env, std::move(mcert));
}

MaybeLocal<Object> X509Certificate::New(
    Environment* env,
    std::shared_ptr<ManagedX509> cert) {
  EscapableHandleScope scope(env->isolate());
  Local<Function> ctor;
  if (!GetConstructorTemplate(env)->GetFunction(env->context()).ToLocal(&ctor))
    return MaybeLocal<Object>();

  Local<Object> obj;
  if (!ctor->NewInstance(env->context()).ToLocal(&obj))
    return MaybeLocal<Object>();

  new X509Certificate(env, obj, std::move(cert));
  return scope.Escape(obj);
}

MaybeLocal<Object> X509Certificate::GetCert(
    Environment* env,
    const SSLPointer& ssl) {
  ClearErrorOnReturn clear_error_on_return;
  X509* cert = SSL_get_certificate(ssl.get());
  if (cert == nullptr)
    return MaybeLocal<Object>();

  X509Pointer ptr(cert);
  return New(env, std::move(ptr));
}

MaybeLocal<Object> X509Certificate::GetPeerCert(
    Environment* env,
    const SSLPointer& ssl,
    GetPeerCertificateFlag flag) {
  EscapableHandleScope scope(env->isolate());
  ClearErrorOnReturn clear_error_on_return;
  Local<Object> obj;
  MaybeLocal<Object> maybe_cert;

  bool is_server =
      static_cast<int>(flag) & static_cast<int>(GetPeerCertificateFlag::SERVER);
  bool abbreviated =
      static_cast<int>(flag)
      & static_cast<int>(GetPeerCertificateFlag::ABBREVIATED);

  X509Pointer cert(is_server ? SSL_get_peer_certificate(ssl.get()) : nullptr);
  STACK_OF(X509)* ssl_certs = SSL_get_peer_cert_chain(ssl.get());
  if (!cert && (ssl_certs == nullptr || sk_X509_num(ssl_certs) == 0))
    return MaybeLocal<Object>();

  std::vector<Local<Value>> certs;

  if (!cert) cert.reset(sk_X509_value(ssl_certs, 0));
  if (!X509Certificate::New(env, std::move(cert)).ToLocal(&obj))
    return MaybeLocal<Object>();

  certs.push_back(obj);

  int count = sk_X509_num(ssl_certs);
  if (!abbreviated) {
    for (int i = 0; i < count; i++) {
      cert.reset(X509_dup(sk_X509_value(ssl_certs, i)));
      if (!cert || !X509Certificate::New(env, std::move(cert)).ToLocal(&obj))
        return MaybeLocal<Object>();
      certs.push_back(obj);
    }
  }

  return scope.Escape(Array::New(env->isolate(), certs.data(), certs.size()));
}

void X509Certificate::Parse(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  CHECK(args[0]->IsArrayBufferView());
  ArrayBufferViewContents<unsigned char> buf(args[0].As<ArrayBufferView>());
  const unsigned char* data = buf.data();
  unsigned data_len = buf.length();

  ClearErrorOnReturn clear_error_on_return;
  BIOPointer bio(LoadBIO(env, args[0]));
  if (!bio)
    return ThrowCryptoError(env, ERR_get_error());

  Local<Object> cert;

  X509Pointer pem(PEM_read_bio_X509_AUX(
      bio.get(), nullptr, NoPasswordCallback, nullptr));
  if (!pem) {
    // Try as DER, but return the original PEM failure if it isn't DER.
    MarkPopErrorOnReturn mark_here;

    X509Pointer der(d2i_X509(nullptr, &data, data_len));
    if (!der)
      return ThrowCryptoError(env, ERR_get_error());

    if (!X509Certificate::New(env, std::move(der)).ToLocal(&cert))
      return;
  } else if (!X509Certificate::New(env, std::move(pem)).ToLocal(&cert)) {
    return;
  }

  args.GetReturnValue().Set(cert);
}

void X509Certificate::Subject(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  BIOPointer bio(BIO_new(BIO_s_mem()));
  Local<Value> ret;
  if (GetSubject(env, bio, cert->get()).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

void X509Certificate::Issuer(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  BIOPointer bio(BIO_new(BIO_s_mem()));
  Local<Value> ret;
  if (GetIssuerString(env, bio, cert->get()).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

void X509Certificate::SubjectAltName(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  BIOPointer bio(BIO_new(BIO_s_mem()));
  Local<Value> ret;
  if (GetInfoString<NID_subject_alt_name>(env, bio, cert->get()).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

void X509Certificate::InfoAccess(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  BIOPointer bio(BIO_new(BIO_s_mem()));
  Local<Value> ret;
  if (GetInfoString<NID_info_access>(env, bio, cert->get()).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

void X509Certificate::ValidFrom(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  BIOPointer bio(BIO_new(BIO_s_mem()));
  Local<Value> ret;
  if (GetValidFrom(env, cert->get(), bio).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

void X509Certificate::ValidTo(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  BIOPointer bio(BIO_new(BIO_s_mem()));
  Local<Value> ret;
  if (GetValidTo(env, cert->get(), bio).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

void X509Certificate::Fingerprint(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  Local<Value> ret;
  if (GetFingerprintDigest(env, EVP_sha1(), cert->get()).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

void X509Certificate::Fingerprint256(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  Local<Value> ret;
  if (GetFingerprintDigest(env, EVP_sha256(), cert->get()).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

void X509Certificate::KeyUsage(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  Local<Value> ret;
  if (GetKeyUsage(env, cert->get()).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

void X509Certificate::SerialNumber(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  Local<Value> ret;
  if (GetSerialNumber(env, cert->get()).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

void X509Certificate::Raw(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  Local<Value> ret;
  if (GetRawDERCertificate(env, cert->get()).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

void X509Certificate::PublicKey(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());

  EVPKeyPointer pkey(X509_get_pubkey(cert->get()));
  ManagedEVPPKey epkey(std::move(pkey));
  std::shared_ptr<KeyObjectData> key_data =
      KeyObjectData::CreateAsymmetric(kKeyTypePublic, epkey);

  Local<Value> ret;
  if (KeyObjectHandle::Create(env, key_data).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

void X509Certificate::Pem(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  BIOPointer bio(BIO_new(BIO_s_mem()));
  if (PEM_write_bio_X509(bio.get(), cert->get()))
    args.GetReturnValue().Set(ToV8Value(env, bio));
}

void X509Certificate::CheckCA(const FunctionCallbackInfo<Value>& args) {
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  args.GetReturnValue().Set(X509_check_ca(cert->get()) == 1);
}

void X509Certificate::CheckHost(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());

  CHECK(args[0]->IsString());  // name
  CHECK(args[1]->IsUint32());  // flags

  Utf8Value name(env->isolate(), args[0]);
  uint32_t flags = args[1].As<Uint32>()->Value();
  char* peername;

  switch (X509_check_host(
              cert->get(),
              *name,
              name.length(),
              flags,
              &peername)) {
    case 1:  {  // Match!
      Local<Value> ret = args[0];
      if (peername != nullptr) {
        ret = OneByteString(env->isolate(), peername);
        OPENSSL_free(peername);
      }
      return args.GetReturnValue().Set(ret);
    }
    case 0:  // No Match!
      return;  // No return value is set
    case -2:  // Error!
      return THROW_ERR_INVALID_ARG_VALUE(env, "Invalid name");
    default:  // Error!
      return THROW_ERR_CRYPTO_OPERATION_FAILED(env);
  }
}

void X509Certificate::CheckEmail(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());

  CHECK(args[0]->IsString());  // name
  CHECK(args[1]->IsUint32());  // flags

  Utf8Value name(env->isolate(), args[0]);
  uint32_t flags = args[1].As<Uint32>()->Value();

  switch (X509_check_email(
              cert->get(),
              *name,
              name.length(),
              flags)) {
    case 1:  // Match!
      return args.GetReturnValue().Set(args[0]);
    case 0:  // No Match!
      return;  // No return value is set
    case -2:  // Error!
      return THROW_ERR_INVALID_ARG_VALUE(env, "Invalid name");
    default:  // Error!
      return THROW_ERR_CRYPTO_OPERATION_FAILED(env);
  }
}

void X509Certificate::CheckIP(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());

  CHECK(args[0]->IsString());  // IP
  CHECK(args[1]->IsUint32());  // flags

  Utf8Value name(env->isolate(), args[0]);
  uint32_t flags = args[1].As<Uint32>()->Value();

  switch (X509_check_ip_asc(cert->get(), *name, flags)) {
    case 1:  // Match!
      return args.GetReturnValue().Set(args[0]);
    case 0:  // No Match!
      return;  // No return value is set
    case -2:  // Error!
      return THROW_ERR_INVALID_ARG_VALUE(env, "Invalid IP");
    default:  // Error!
      return THROW_ERR_CRYPTO_OPERATION_FAILED(env);
  }
}

void X509Certificate::CheckIssued(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());

  CHECK(args[0]->IsObject());
  CHECK(X509Certificate::HasInstance(env, args[0].As<Object>()));

  X509Certificate* issuer;
  ASSIGN_OR_RETURN_UNWRAP(&issuer, args[0]);

  args.GetReturnValue().Set(
    X509_check_issued(issuer->get(), cert->get()) == X509_V_OK);
}

void X509Certificate::CheckPrivateKey(const FunctionCallbackInfo<Value>& args) {
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());

  CHECK(args[0]->IsObject());
  KeyObjectHandle* key;
  ASSIGN_OR_RETURN_UNWRAP(&key, args[0]);
  CHECK_EQ(key->Data()->GetKeyType(), kKeyTypePrivate);

  args.GetReturnValue().Set(
      X509_check_private_key(
          cert->get(),
          key->Data()->GetAsymmetricKey().get()) == 1);
}

void X509Certificate::Verify(const FunctionCallbackInfo<Value>& args) {
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());

  CHECK(args[0]->IsObject());
  KeyObjectHandle* key;
  ASSIGN_OR_RETURN_UNWRAP(&key, args[0]);
  CHECK_EQ(key->Data()->GetKeyType(), kKeyTypePublic);

  args.GetReturnValue().Set(
      X509_verify(
          cert->get(),
          key->Data()->GetAsymmetricKey().get()) > 0);
}

void X509Certificate::ToLegacy(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  X509Certificate* cert;
  ASSIGN_OR_RETURN_UNWRAP(&cert, args.Holder());
  Local<Value> ret;
  if (X509ToObject(env, cert->get()).ToLocal(&ret))
    args.GetReturnValue().Set(ret);
}

X509Certificate::X509Certificate(
    Environment* env,
    Local<Object> object,
    std::shared_ptr<ManagedX509> cert)
    : BaseObject(env, object),
      cert_(std::move(cert)) {
  MakeWeak();
}

void X509Certificate::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackField("cert", cert_);
}

BaseObjectPtr<BaseObject>
X509Certificate::X509CertificateTransferData::Deserialize(
    Environment* env,
    Local<Context> context,
    std::unique_ptr<worker::TransferData> self) {
  if (context != env->context()) {
    THROW_ERR_MESSAGE_TARGET_CONTEXT_UNAVAILABLE(env);
    return {};
  }

  Local<Value> handle;
  if (!X509Certificate::New(env, data_).ToLocal(&handle))
    return {};

  return BaseObjectPtr<BaseObject>(
      Unwrap<X509Certificate>(handle.As<Object>()));
}


BaseObject::TransferMode X509Certificate::GetTransferMode() const {
  return BaseObject::TransferMode::kCloneable;
}

std::unique_ptr<worker::TransferData> X509Certificate::CloneForMessaging()
    const {
  return std::make_unique<X509CertificateTransferData>(cert_);
}


void X509Certificate::Initialize(Environment* env, Local<Object> target) {
  env->SetMethod(target, "parseX509", X509Certificate::Parse);

  NODE_DEFINE_CONSTANT(target, X509_CHECK_FLAG_ALWAYS_CHECK_SUBJECT);
  NODE_DEFINE_CONSTANT(target, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);
  NODE_DEFINE_CONSTANT(target, X509_CHECK_FLAG_NO_WILDCARDS);
  NODE_DEFINE_CONSTANT(target, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
  NODE_DEFINE_CONSTANT(target, X509_CHECK_FLAG_MULTI_LABEL_WILDCARDS);
  NODE_DEFINE_CONSTANT(target, X509_CHECK_FLAG_SINGLE_LABEL_SUBDOMAINS);
}

}  // namespace crypto
}  // namespace node