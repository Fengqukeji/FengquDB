#include "fengqudb_store.h"
#include <jni.h>
#include <string>
#include <limits>
#include <memory>
#include <vector>
#include <array>

namespace {
template <class T> T *ptr(jlong value) {
    return reinterpret_cast<T *>(static_cast<intptr_t>(value));
}
template <class T> jlong id(T *value) {
    return static_cast<jlong>(reinterpret_cast<intptr_t>(value));
}
bool check(JNIEnv *env, fqdb_status status) {
    if (!status)
        return true;
    if (env->ExceptionCheck())
        return false;
    auto type = env->FindClass("io/fengqudb/Store$DbException");
    if (type) {
        auto ctor = env->GetMethodID(type, "<init>", "(I)V");
        if (ctor) {
            auto error = static_cast<jthrowable>(env->NewObject(type, ctor, status));
            if (error) {
                env->Throw(error);
                env->DeleteLocalRef(error);
            }
        }
        env->DeleteLocalRef(type);
    }
    return false;
}
template <class R, class F> R guard(JNIEnv *env, R fallback, F &&function) {
    try {
        return function();
    } catch (const std::bad_alloc &) {
        if (!env->ExceptionCheck()) {
            auto type = env->FindClass("java/lang/OutOfMemoryError");
            if (type) {
                env->ThrowNew(type, "Native allocation failed");
                env->DeleteLocalRef(type);
            }
        }
    } catch (...) {
        check(env, FQDB_INTERNAL_ERROR);
    }
    return fallback;
}
class Input {
    JNIEnv *env_;
    jbyteArray array_;
    jbyte *data_ = nullptr;
    jsize size_ = 0;
    bool valid_ = false;

  public:
    Input(JNIEnv *env, jbyteArray array) : env_(env), array_(array) {
        if (!array) {
            check(env, FQDB_INVALID_ARGUMENT);
            return;
        }
        size_ = env->GetArrayLength(array);
        if (size_)
            data_ = env->GetByteArrayElements(array, nullptr);
        valid_ = !size_ || data_;
    }
    ~Input() {
        if (data_)
            env_->ReleaseByteArrayElements(array_, data_, JNI_ABORT);
    }
    bool valid() const {
        return valid_;
    }
    fqdb_slice slice() const {
        return {data_, static_cast<uint64_t>(size_)};
    }
};
jbyteArray copy(JNIEnv *env, fqdb_slice slice) {
    if (slice.size > static_cast<uint64_t>(std::numeric_limits<jsize>::max())) {
        check(env, FQDB_LIMIT);
        return nullptr;
    }
    auto result = env->NewByteArray(static_cast<jsize>(slice.size));
    if (result && slice.size)
        env->SetByteArrayRegion(result, 0, static_cast<jsize>(slice.size),
                                static_cast<const jbyte *>(slice.data));
    return result;
}
bool directory(JNIEnv *env, jbyteArray path, std::string &result) {
    Input input(env, path);
    if (!input.valid())
        return false;
    auto bytes = input.slice();
    if (bytes.size)
        result.assign(static_cast<const char *>(bytes.data), static_cast<size_t>(bytes.size));
    if (result.empty() || result.find('\0') != std::string::npos)
        return check(env, FQDB_INVALID_ARGUMENT);
    return true;
}
bool maintenance_options(JNIEnv *env, jlongArray limits, fqdb_store_maintenance_options &output,
                         fqdb_store_maintenance_io *stats = nullptr) {
    if (!limits || (env->GetArrayLength(limits) != 3 && env->GetArrayLength(limits) != 5))
        return check(env, FQDB_INVALID_ARGUMENT);
    jlong values[5]{};
    env->GetLongArrayRegion(limits, 0, env->GetArrayLength(limits), values);
    if (env->ExceptionCheck())
        return false;
    if (values[0] < 0 || values[0] > 64 || values[1] < 0 || values[2] < 0 || values[3] < 0 ||
        values[4] < 0)
        return check(env, FQDB_INVALID_ARGUMENT);
    output = {sizeof(output),
              values[3] || values[4] || stats ? 2u : 1u,
              static_cast<uint32_t>(values[0]),
              0,
              static_cast<uint64_t>(values[1]),
              static_cast<uint64_t>(values[2]),
              static_cast<uint64_t>(values[3]),
              static_cast<uint64_t>(values[4]),
              stats};
    return true;
}
void maintenance_info(JNIEnv *env, const fqdb_store_maintenance_info &info, jlongArray result) {
    const uint64_t fields[]{info.snapshot_version,  info.final_version, info.source_file_bytes,
                            info.output_file_bytes, info.stored_keys,   info.stored_value_bytes,
                            info.copied_keys,       info.catchup_rounds};
    jlong values[8];
    for (size_t i = 0; i < 8; ++i)
        values[i] = static_cast<jlong>(fields[i]);
    env->SetLongArrayRegion(result, 0, 8, values);
}
void maintenance_io(JNIEnv *env, const fqdb_store_maintenance_io &io, jlongArray result) {
    if (env->GetArrayLength(result) == 11) {
        jlong values[]{static_cast<jlong>(io.read_bytes), static_cast<jlong>(io.write_bytes),
                       static_cast<jlong>(io.sync_calls)};
        env->SetLongArrayRegion(result, 8, 3, values);
    }
}
class ArrayInputs {
    JNIEnv *env_;
    std::vector<jbyteArray> arrays_;
    std::vector<std::unique_ptr<Input>> values_;
    bool valid_ = false;

  public:
    std::vector<fqdb_slice> slices;
    ArrayInputs(JNIEnv *env, jobjectArray arrays) : env_(env) {
        if (!arrays) {
            check(env, FQDB_INVALID_ARGUMENT);
            return;
        }
        auto count = env->GetArrayLength(arrays);
        if (count > 4096) {
            check(env, FQDB_INVALID_ARGUMENT);
            return;
        }
        if (env->EnsureLocalCapacity(count + 16) < 0)
            return;
        arrays_.reserve(count);
        values_.reserve(count);
        slices.reserve(count);
        for (jsize i = 0; i < count; ++i) {
            auto array = static_cast<jbyteArray>(env->GetObjectArrayElement(arrays, i));
            if (!array) {
                check(env, FQDB_INVALID_ARGUMENT);
                return;
            }
            arrays_.push_back(array);
            auto value = std::make_unique<Input>(env, array);
            if (!value->valid())
                return;
            slices.push_back(value->slice());
            values_.push_back(std::move(value));
        }
        valid_ = true;
    }
    ~ArrayInputs() {
        values_.clear();
        for (auto array : arrays_)
            env_->DeleteLocalRef(array);
    }
    bool valid() const {
        return valid_;
    }
};
bool projection_fields(JNIEnv *env, jintArray input, std::vector<uint32_t> &fields) {
    if (!input || env->GetArrayLength(input) > 256) {
        check(env, FQDB_INVALID_ARGUMENT);
        return false;
    }
    const auto count = env->GetArrayLength(input);
    std::array<jint, 256> values{};
    env->GetIntArrayRegion(input, 0, count, values.data());
    if (env->ExceptionCheck())
        return false;
    fields.reserve(count);
    for (jsize i = 0; i < count; ++i) {
        if (values[i] <= 0 || values[i] > 0x1fffffff) {
            check(env, FQDB_INVALID_ARGUMENT);
            return false;
        }
        fields.push_back(static_cast<uint32_t>(values[i]));
    }
    return true;
}
jobject entry(JNIEnv *env, fqdb_slice value, uint64_t version, uint64_t expires,
              jclass type = nullptr, jmethodID ctor = nullptr) {
    if (!type)
        type = env->FindClass("io/fengqudb/Store$Entry");
    if (!type)
        return nullptr;
    if (!ctor)
        ctor = env->GetMethodID(type, "<init>", "([BJJ)V");
    if (!ctor)
        return nullptr;
    auto bytes = copy(env, value);
    if (!bytes)
        return nullptr;
    auto result =
        env->NewObject(type, ctor, bytes, static_cast<jlong>(version), static_cast<jlong>(expires));
    env->DeleteLocalRef(bytes);
    return result;
}
} // namespace
extern "C" {
static jlong open_store(JNIEnv *env, jbyteArray path, jbyteArray database_name, jboolean buffered,
                        jboolean create, jlongArray limits) {
    return guard<jlong>(env, 0, [&]() -> jlong {
        Input input(env, path);
        if (!input.valid())
            return 0;
        const auto count = limits ? env->GetArrayLength(limits) : 0;
        if (count != 7 && count != 8) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        // Keep the seven-field contract used by previously compiled classes.
        jlong values[8]{};
        env->GetLongArrayRegion(limits, 0, count, values);
        if (env->ExceptionCheck())
            return 0;
        for (auto value : values)
            if (value < 0) {
                check(env, FQDB_INVALID_ARGUMENT);
                return 0;
            }
        if (values[7] > 1) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        auto bytes = input.slice();
        std::string name;
        if (bytes.size)
            name.assign(static_cast<const char *>(bytes.data), static_cast<size_t>(bytes.size));
        if (name.find('\0') != std::string::npos) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        fqdb_store_options options{
            sizeof(options),
            1,
            buffered ? 1u : 0u,
            (create ? static_cast<uint32_t>(FQDB_STORE_CREATE) : 0u) |
                (values[7] ? static_cast<uint32_t>(FQDB_STORE_SEGMENTED) : 0u),
            static_cast<uint64_t>(values[0]),
            static_cast<uint64_t>(values[1]),
            static_cast<uint64_t>(values[2]),
            static_cast<uint64_t>(values[3]),
            static_cast<uint64_t>(values[4]),
            static_cast<uint64_t>(values[5]),
            static_cast<uint64_t>(values[6])};
        fqdb_store *output = nullptr;
        std::string short_name;
        if (database_name && !directory(env, database_name, short_name))
            return 0;
        auto status = database_name
                          ? fqdb_store_open_at(name.c_str(), short_name.c_str(), &options, &output)
                          : fqdb_store_open(name.c_str(), &options, &output);
        if (!check(env, status))
            return 0;
        return id(output);
    });
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nOpen(JNIEnv *env, jclass, jbyteArray path,
                                                     jboolean buffered, jboolean create,
                                                     jlongArray limits) {
    return open_store(env, path, nullptr, buffered, create, limits);
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nOpenAt(JNIEnv *env, jclass, jbyteArray root,
                                                       jbyteArray name, jboolean buffered,
                                                       jboolean create, jlongArray limits) {
    return open_store(env, root, name, buffered, create, limits);
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nWatchOpen(JNIEnv *env, jclass, jlong handle,
                                                          jlong from, jbyteArray prefix,
                                                          jlongArray options) {
    return guard<jlong>(env, 0, [&]() -> jlong {
        if (!options || env->GetArrayLength(options) != 3) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        jlong limits[3];
        env->GetLongArrayRegion(options, 0, 3, limits);
        if (env->ExceptionCheck())
            return 0;
        if (limits[0] < 0 || limits[0] > 3 || limits[1] < 1 || limits[1] > 4096 || limits[2] < 0) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        Input key(env, prefix);
        if (!key.valid())
            return 0;
        fqdb_store_watch_options settings{sizeof(settings), 1, static_cast<uint32_t>(limits[0]),
                                          static_cast<uint32_t>(limits[1]),
                                          static_cast<uint64_t>(limits[2])};
        fqdb_store_watch *watch = nullptr;
        if (!check(env, fqdb_store_watch_open(ptr<fqdb_store>(handle), static_cast<uint64_t>(from),
                                              key.slice(), &settings, &watch)))
            return 0;
        return id(watch);
    });
}
JNIEXPORT jobjectArray JNICALL Java_io_fengqudb_Store_nWatchNext(JNIEnv *env, jclass, jlong handle,
                                                                 jlong timeout) {
    return guard<jobjectArray>(env, nullptr, [&]() -> jobjectArray {
        fqdb_store_change_batch *raw = nullptr;
        auto status = fqdb_store_watch_next(ptr<fqdb_store_watch>(handle),
                                            static_cast<uint64_t>(timeout), &raw);
        if (status == FQDB_NOT_MODIFIED || status == FQDB_TIMEOUT)
            return nullptr;
        if (!check(env, status))
            return nullptr;
        std::unique_ptr<fqdb_store_change_batch, decltype(&fqdb_store_change_batch_close)> batch(
            raw, fqdb_store_change_batch_close);
        const auto &data = *fqdb_store_change_batch_data(raw);
        if (data.count > static_cast<uint32_t>((std::numeric_limits<jsize>::max() - 4) / 2)) {
            check(env, FQDB_LIMIT);
            return nullptr;
        }
        auto byte_type = env->FindClass("[B");
        if (!byte_type)
            return nullptr;
        auto bytes = env->NewObjectArray(static_cast<jsize>(2 * data.count), byte_type, nullptr);
        env->DeleteLocalRef(byte_type);
        if (!bytes)
            return nullptr;
        std::vector<jlong> numbers(4 + 2 * data.count);
        numbers[0] = static_cast<jlong>(data.version);
        numbers[1] = static_cast<jlong>(data.unix_ms);
        numbers[2] = static_cast<jlong>(data.durable_version);
        numbers[3] = data.flags;
        for (uint32_t i = 0; i < data.count; ++i) {
            const auto &row = data.rows[i];
            auto key = copy(env, row.key);
            if (!key)
                return nullptr;
            env->SetObjectArrayElement(bytes, static_cast<jsize>(2 * i), key);
            env->DeleteLocalRef(key);
            if (env->ExceptionCheck())
                return nullptr;
            if (row.kind == FQDB_PUT && !(data.flags & FQDB_WATCH_METADATA_ONLY)) {
                auto value = copy(env, row.value);
                if (!value)
                    return nullptr;
                env->SetObjectArrayElement(bytes, static_cast<jsize>(2 * i + 1), value);
                env->DeleteLocalRef(value);
                if (env->ExceptionCheck())
                    return nullptr;
            }
            numbers[4 + 2 * i] = static_cast<jlong>(row.expires_at_ms);
            numbers[5 + 2 * i] = row.kind;
        }
        auto metadata = env->NewLongArray(static_cast<jsize>(numbers.size()));
        if (!metadata)
            return nullptr;
        env->SetLongArrayRegion(metadata, 0, static_cast<jsize>(numbers.size()), numbers.data());
        if (env->ExceptionCheck())
            return nullptr;
        auto type = env->FindClass("java/lang/Object");
        if (!type)
            return nullptr;
        auto result = env->NewObjectArray(2, type, nullptr);
        env->DeleteLocalRef(type);
        if (result) {
            env->SetObjectArrayElement(result, 0, bytes);
            env->SetObjectArrayElement(result, 1, metadata);
        }
        return result;
    });
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nWatchAck(JNIEnv *env, jclass, jlong handle,
                                                        jlong version) {
    check(env, fqdb_store_watch_ack(ptr<fqdb_store_watch>(handle), static_cast<uint64_t>(version)));
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nWatchCancel(JNIEnv *, jclass, jlong handle) {
    fqdb_store_watch_cancel(ptr<fqdb_store_watch>(handle));
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nClose(JNIEnv *, jclass, jlong handle, jint kind) {
    switch (kind) {
    case 0:
        fqdb_store_close(ptr<fqdb_store>(handle));
        break;
    case 1:
        fqdb_store_txn_close(ptr<fqdb_store_txn>(handle));
        break;
    case 2:
        fqdb_store_snapshot_close(ptr<fqdb_store_snapshot>(handle));
        break;
    case 3:
        fqdb_store_cursor_close(ptr<fqdb_store_cursor>(handle));
        break;
    case 4:
        fqdb_store_key_close(ptr<fqdb_store_key>(handle));
        break;
    case 5:
        fqdb_store_batch_close(ptr<fqdb_store_batch>(handle));
        break;
    case 6:
        fqdb_store_watch_close(ptr<fqdb_store_watch>(handle));
        break;
    case 7:
        fqdb_store_value_close(ptr<fqdb_store_value>(handle));
        break;
    case 8:
        fqdb_store_results_close(ptr<fqdb_store_results>(handle));
        break;
    case 9:
        fqdb_store_reader_close(ptr<fqdb_store_reader>(handle));
        break;
    case 10:
        fqdb_store_commit_close(ptr<fqdb_store_commit>(handle));
        break;
    }
}
JNIEXPORT jbyteArray JNICALL Java_io_fengqudb_Store_nPathAt(JNIEnv *env, jclass, jbyteArray root,
                                                            jbyteArray name) {
    return guard<jbyteArray>(env, nullptr, [&]() -> jbyteArray {
        std::string a, b;
        if (!directory(env, root, a) || !directory(env, name, b))
            return nullptr;
        fqdb_buffer output{};
        if (!check(env, fqdb_store_path_at(a.c_str(), b.c_str(), &output)))
            return nullptr;
        auto result = copy(env, {output.data, output.size});
        fqdb_buffer_free(&output);
        return result;
    });
}
JNIEXPORT jobject JNICALL Java_io_fengqudb_Store_nGetEntry(JNIEnv *env, jclass, jlong handle,
                                                           jbyteArray key, jint kind) {
    return guard<jobject>(env, nullptr, [&]() -> jobject {
        fqdb_store_value *output = nullptr;
        fqdb_status status;
        if (kind == 4)
            status = fqdb_store_key_get_view(ptr<fqdb_store_key>(handle), &output);
        else {
            Input input(env, key);
            if (!input.valid())
                return nullptr;
            status = kind == 0
                         ? fqdb_store_get_view(ptr<fqdb_store>(handle), input.slice(), &output)
                     : kind == 2 ? fqdb_store_snapshot_get_view(ptr<fqdb_store_snapshot>(handle),
                                                                input.slice(), &output)
                                 : FQDB_INVALID_ARGUMENT;
        }
        if (status == FQDB_NOT_FOUND)
            return nullptr;
        if (!check(env, status))
            return nullptr;
        std::unique_ptr<fqdb_store_value, decltype(&fqdb_store_value_close)> keep(
            output, fqdb_store_value_close);
        return entry(env, fqdb_store_value_data(output), fqdb_store_value_version(output),
                     fqdb_store_value_expires_at_ms(output));
    });
}
JNIEXPORT jobjectArray JNICALL Java_io_fengqudb_Store_nGetMany(JNIEnv *env, jclass, jlong handle,
                                                               jobjectArray keys, jint kind,
                                                               jboolean metadata) {
    return guard<jobjectArray>(env, nullptr, [&]() -> jobjectArray {
        ArrayInputs input(env, keys);
        if (!input.valid())
            return nullptr;
        fqdb_store_results *output = nullptr;
        auto count = static_cast<uint32_t>(input.slices.size());
        auto status =
            kind == 0
                ? fqdb_store_get_many(ptr<fqdb_store>(handle), input.slices.data(), count, &output)
            : kind == 1 ? fqdb_store_txn_get_many(ptr<fqdb_store_txn>(handle), input.slices.data(),
                                                  count, &output)
            : kind == 2 ? fqdb_store_snapshot_get_many(ptr<fqdb_store_snapshot>(handle),
                                                       input.slices.data(), count, &output)
                        : FQDB_INVALID_ARGUMENT;
        if (!check(env, status))
            return nullptr;
        std::unique_ptr<fqdb_store_results, decltype(&fqdb_store_results_close)> keep(
            output, fqdb_store_results_close);
        auto type = env->FindClass(metadata ? "io/fengqudb/Store$Entry" : "[B");
        if (!type)
            return nullptr;
        auto ctor = metadata ? env->GetMethodID(type, "<init>", "([BJJ)V") : nullptr;
        if (metadata && !ctor)
            return nullptr;
        auto result = env->NewObjectArray(static_cast<jsize>(count), type, nullptr);
        if (!result)
            return nullptr;
        auto rows = fqdb_store_results_data(output);
        for (uint32_t i = 0; i < count; ++i)
            if (rows[i].found) {
                jobject value = metadata ? entry(env, rows[i].value, rows[i].version,
                                                 rows[i].expires_at_ms, type, ctor)
                                         : copy(env, rows[i].value);
                if (!value)
                    return nullptr;
                env->SetObjectArrayElement(result, static_cast<jsize>(i), value);
                env->DeleteLocalRef(value);
                if (env->ExceptionCheck())
                    return nullptr;
            }
        return result;
    });
}
JNIEXPORT jbyteArray JNICALL Java_io_fengqudb_Store_nKeyGet(JNIEnv *env, jclass, jlong handle) {
    return guard<jbyteArray>(env, nullptr, [&]() -> jbyteArray {
        fqdb_store_value *out = nullptr;
        auto status = fqdb_store_key_get_view(ptr<fqdb_store_key>(handle), &out);
        if (status == FQDB_NOT_FOUND)
            return nullptr;
        if (!check(env, status))
            return nullptr;
        std::unique_ptr<fqdb_store_value, decltype(&fqdb_store_value_close)> keep(
            out, fqdb_store_value_close);
        return copy(env, fqdb_store_value_data(out));
    });
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nPrepareKey(JNIEnv *env, jclass, jlong handle,
                                                           jbyteArray key) {
    return guard<jlong>(env, 0, [&]() -> jlong {
        Input input(env, key);
        if (!input.valid())
            return 0;
        fqdb_store_key *out = nullptr;
        if (!check(env, fqdb_store_key_prepare(ptr<fqdb_store>(handle), input.slice(), &out)))
            return 0;
        return id(out);
    });
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nKeyGetInto(JNIEnv *env, jclass, jlong handle,
                                                           jbyteArray destination) {
    return guard<jlong>(env, -1, [&]() -> jlong {
        if (!destination) {
            check(env, FQDB_INVALID_ARGUMENT);
            return -1;
        }
        auto size = env->GetArrayLength(destination);
        auto data = size ? env->GetByteArrayElements(destination, nullptr) : nullptr;
        if (size && !data)
            return -1;
        uint64_t required = 0;
        auto status = fqdb_store_key_get_into(ptr<fqdb_store_key>(handle), data,
                                              static_cast<uint64_t>(size), &required);
        if (data)
            env->ReleaseByteArrayElements(destination, data, status == FQDB_OK ? 0 : JNI_ABORT);
        if (status == FQDB_NOT_FOUND)
            return -1;
        return check(env, status) ? static_cast<jlong>(required) : -1;
    });
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nKeyGetIntoDirect(JNIEnv *env, jclass, jlong handle,
                                                                 jobject destination, jint offset,
                                                                 jint size) {
    return guard<jlong>(env, -1, [&]() -> jlong {
        if (!destination) {
            check(env, FQDB_INVALID_ARGUMENT);
            return -1;
        }
        auto capacity = env->GetDirectBufferCapacity(destination);
        auto data = static_cast<unsigned char *>(env->GetDirectBufferAddress(destination));
        if (capacity < 0 || offset < 0 || size < 0 || offset > capacity ||
            size > capacity - offset || (capacity && !data)) {
            check(env, FQDB_INVALID_ARGUMENT);
            return -1;
        }
        uint64_t required = 0;
        // The JNI local reference keeps direct memory alive through the call.
        auto status =
            fqdb_store_key_get_into(ptr<fqdb_store_key>(handle), data ? data + offset : nullptr,
                                    static_cast<uint64_t>(size), &required);
        if (status == FQDB_NOT_FOUND)
            return -1;
        return check(env, status) ? static_cast<jlong>(required) : -1;
    });
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nBulkWrite(JNIEnv *env, jclass, jlong handle,
                                                          jint operation, jobjectArray keys,
                                                          jobjectArray values, jlongArray expires) {
    return guard<jlong>(env, 0, [&]() -> jlong {
        ArrayInputs input(env, keys);
        if (!input.valid())
            return 0;
        auto count = static_cast<uint32_t>(input.slices.size());
        if (operation == 1) {
            check(env, fqdb_store_txn_delete_many(ptr<fqdb_store_txn>(handle), input.slices.data(),
                                                  count));
            return 0;
        }
        if (operation != 0 && operation != 2) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        ArrayInputs data(env, values);
        if (!data.valid())
            return 0;
        if (data.slices.size() != count || !expires ||
            env->GetArrayLength(expires) != static_cast<jsize>(count)) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        std::vector<jlong> times(count);
        if (count)
            env->GetLongArrayRegion(expires, 0, static_cast<jsize>(count), times.data());
        if (env->ExceptionCheck())
            return 0;
        std::vector<fqdb_store_put_item> items;
        items.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (times[i] < 0) {
                check(env, FQDB_INVALID_ARGUMENT);
                return 0;
            }
            items.push_back({input.slices[i], data.slices[i], static_cast<uint64_t>(times[i])});
        }
        if (operation == 0) {
            check(env, fqdb_store_txn_put_many(ptr<fqdb_store_txn>(handle), items.data(), count));
            return 0;
        }
        fqdb_store_batch *out = nullptr;
        if (!check(env,
                   fqdb_store_batch_create(ptr<fqdb_store>(handle), items.data(), count, &out)))
            return 0;
        return id(out);
    });
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nPutBatch(JNIEnv *env, jclass, jlong txn,
                                                        jlong batch) {
    check(env, fqdb_store_txn_put_batch(ptr<fqdb_store_txn>(txn), ptr<fqdb_store_batch>(batch)));
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nAtomic(JNIEnv *env, jclass, jlong handle,
                                                      jint operation, jbyteArray key,
                                                      jbyteArray value, jlongArray input,
                                                      jlongArray output, jboolean force) {
    guard<int>(env, 0, [&] {
        if (!input || env->GetArrayLength(input) != 6 || !output ||
            env->GetArrayLength(output) != 5) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        jlong values[6]{};
        env->GetLongArrayRegion(input, 0, 6, values);
        if (env->ExceptionCheck())
            return 0;
        if (values[1] < 0 || values[5] < 0) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        Input k(env, key);
        if (!k.valid())
            return 0;
        std::unique_ptr<Input> v;
        if (operation == 0 || operation == 2) {
            v = std::make_unique<Input>(env, value);
            if (!v->valid())
                return 0;
        }
        fqdb_store_atomic_info info{};
        fqdb_status status = FQDB_INVALID_ARGUMENT;
        auto store = ptr<fqdb_store>(handle);
        auto timeout = static_cast<uint64_t>(values[5]);
        if (operation == 0)
            status = fqdb_store_compare_exchange(store, k.slice(), static_cast<uint64_t>(values[0]),
                                                 v->slice(), static_cast<uint64_t>(values[1]),
                                                 timeout, &info, force ? 1u : 0u);
        else if (operation == 1)
            status =
                fqdb_store_delete_if_version(store, k.slice(), static_cast<uint64_t>(values[0]),
                                             timeout, &info, force ? 1u : 0u);
        else if (operation == 2)
            status = fqdb_store_put_if_changed(store, k.slice(), v->slice(),
                                               static_cast<uint64_t>(values[1]), timeout, &info,
                                               force ? 1u : 0u);
        else if (operation == 3)
            status = fqdb_store_add_i64(store, k.slice(), values[2], values[3], values[4], timeout,
                                        &info, force ? 1u : 0u);
        else if (operation == 4)
            status = fqdb_store_history_hold(store, k.slice(), static_cast<uint64_t>(values[0]),
                                             timeout, &info);
        else if (operation == 5)
            status = fqdb_store_history_release(store, k.slice(), timeout, &info);
        if (!check(env, status))
            return 0;
        jlong result[5]{static_cast<jlong>(info.version), static_cast<jlong>(info.observed_version),
                        info.value, info.matched, info.changed};
        env->SetLongArrayRegion(output, 0, 5, result);
        return 0;
    });
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nAddInt64Fields(JNIEnv *env, jclass, jlong handle,
                                                              jobjectArray keys, jlongArray input,
                                                              jlong timeout, jlongArray output,
                                                              jboolean force) {
    guard<int>(env, 0, [&] {
        if (!keys || !input || !output || timeout < 0 || env->GetArrayLength(keys) < 1 ||
            env->GetArrayLength(keys) > 256 ||
            env->GetArrayLength(input) != env->GetArrayLength(keys) * 4 ||
            env->GetArrayLength(output) != 5) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        ArrayInputs names(env, keys);
        if (!names.valid())
            return 0;
        const auto count = names.slices.size();
        std::vector<jlong> values(count * 4);
        env->GetLongArrayRegion(input, 0, static_cast<jsize>(values.size()), values.data());
        if (env->ExceptionCheck())
            return 0;
        std::vector<fqdb_store_i64_field_update> items;
        items.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (values[i * 4] < 0) {
                check(env, FQDB_INVALID_ARGUMENT);
                return 0;
            }
            items.push_back({names.slices[i], static_cast<uint64_t>(values[i * 4]),
                             values[i * 4 + 1], values[i * 4 + 2], values[i * 4 + 3]});
        }
        fqdb_store_atomic_info info{};
        if (!check(env, fqdb_store_add_i64_fields(
                            ptr<fqdb_store>(handle), items.data(), static_cast<uint32_t>(count),
                            static_cast<uint64_t>(timeout), &info, force ? 1u : 0u)))
            return 0;
        const jlong result[]{static_cast<jlong>(info.version),
                             static_cast<jlong>(info.observed_version), info.value, info.matched,
                             info.changed};
        env->SetLongArrayRegion(output, 0, 5, result);
        return 0;
    });
}
JNIEXPORT jboolean JNICALL Java_io_fengqudb_Store_nSharedBufferAliasesRetainRoot(JNIEnv *env,
                                                                                 jclass) {
    // OpenJDK direct aliases retain their root through `att`. Android and other
    // VMs can instead retain a separate memory object, which does NOT retain a
    // JNI-created root. Never attach a native lease to that shorter lifetime.
    static const uint64_t probe = 0;
    auto root = env->NewDirectByteBuffer(const_cast<uint64_t *>(&probe), sizeof(probe));
    if (!root)
        return JNI_FALSE;
    auto current = root;
    bool supported = true;
    for (auto method : {"asReadOnlyBuffer", "duplicate", "slice", "asReadOnlyBuffer"}) {
        auto type = env->GetObjectClass(current);
        auto call = type ? env->GetMethodID(type, method, "()Ljava/nio/ByteBuffer;") : nullptr;
        auto next = call ? env->CallObjectMethod(current, call) : nullptr;
        if (type)
            env->DeleteLocalRef(type);
        if (!next || env->ExceptionCheck()) {
            supported = false;
            break;
        }
        auto previous = current;
        current = next;
        type = env->GetObjectClass(current);
        auto field = type ? env->GetFieldID(type, "att", "Ljava/lang/Object;") : nullptr;
        auto attachment = field ? env->GetObjectField(current, field) : nullptr;
        if (type)
            env->DeleteLocalRef(type);
        supported =
            attachment && !env->ExceptionCheck() &&
            (env->IsSameObject(attachment, root) || env->IsSameObject(attachment, previous));
        if (attachment)
            env->DeleteLocalRef(attachment);
        if (previous != root)
            env->DeleteLocalRef(previous);
        if (!supported)
            break;
    }
    if (current != root)
        env->DeleteLocalRef(current);
    env->DeleteLocalRef(root);
    // Missing implementation fields mean use the safe copying path.
    if (env->ExceptionCheck())
        env->ExceptionClear();
    return supported ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jobject JNICALL Java_io_fengqudb_Store_nGetShared(JNIEnv *env, jclass, jlong handle,
                                                            jbyteArray key, jint kind,
                                                            jlongArray metadata) {
    return guard<jobject>(env, nullptr, [&]() -> jobject {
        if (!metadata || env->GetArrayLength(metadata) != 3 || (kind != 0 && kind != 4)) {
            check(env, FQDB_INVALID_ARGUMENT);
            return nullptr;
        }
        fqdb_store_shared_value out{};
        fqdb_status status;
        if (kind == 4)
            status = fqdb_store_key_get_shared(ptr<fqdb_store_key>(handle), &out);
        else {
            Input input(env, key);
            if (!input.valid())
                return nullptr;
            status = fqdb_store_get_shared(ptr<fqdb_store>(handle), input.slice(), &out);
        }
        if (status == FQDB_NOT_FOUND)
            return nullptr;
        if (!check(env, status))
            return nullptr;
        std::unique_ptr<fqdb_store_value, decltype(&fqdb_store_value_close)> owner(
            out.owner, fqdb_store_value_close);
        auto buffer = env->NewDirectByteBuffer(const_cast<void *>(out.row.value.data),
                                               static_cast<jlong>(out.row.value.size));
        if (!buffer)
            return nullptr;
        jlong fields[]{id(out.owner), static_cast<jlong>(out.row.version),
                       static_cast<jlong>(out.row.expires_at_ms)};
        env->SetLongArrayRegion(metadata, 0, 3, fields);
        if (env->ExceptionCheck())
            return nullptr;
        owner.release();
        return buffer;
    });
}
JNIEXPORT jbyteArray JNICALL Java_io_fengqudb_Store_nProject(JNIEnv *env, jclass, jlong handle,
                                                             jbyteArray key, jintArray fields) {
    return guard<jbyteArray>(env, nullptr, [&]() -> jbyteArray {
        Input name(env, key);
        std::vector<uint32_t> selected;
        if (!name.valid() || !projection_fields(env, fields, selected))
            return nullptr;
        fqdb_buffer result{};
        if (!check(env, fqdb_store_project(ptr<fqdb_store>(handle), name.slice(), selected.data(),
                                           static_cast<uint32_t>(selected.size()), &result)))
            return nullptr;
        struct Free {
            fqdb_buffer &value;
            ~Free() {
                fqdb_buffer_free(&value);
            }
        } free{result};
        return copy(env, {result.data, result.size});
    });
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nProjectReaderCreate(JNIEnv *env, jclass,
                                                                    jlong handle, jobjectArray keys,
                                                                    jintArray fields) {
    return guard<jlong>(env, 0, [&]() -> jlong {
        ArrayInputs names(env, keys);
        std::vector<uint32_t> selected;
        if (!names.valid() || !projection_fields(env, fields, selected))
            return 0;
        fqdb_store_reader *reader = nullptr;
        if (!check(env, fqdb_store_project_reader_create(
                            ptr<fqdb_store>(handle), names.slices.data(),
                            static_cast<uint32_t>(names.slices.size()), selected.data(),
                            static_cast<uint32_t>(selected.size()), &reader)))
            return 0;
        return id(reader);
    });
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nReaderCreate(JNIEnv *env, jclass, jlong handle,
                                                             jobjectArray keys) {
    return guard<jlong>(env, 0, [&]() -> jlong {
        ArrayInputs input(env, keys);
        if (!input.valid())
            return 0;
        fqdb_store_reader *out = nullptr;
        if (!check(env, fqdb_store_reader_create(ptr<fqdb_store>(handle), input.slices.data(),
                                                 static_cast<uint32_t>(input.slices.size()), &out)))
            return 0;
        return id(out);
    });
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nReaderAcquire(JNIEnv *env, jclass, jlong handle,
                                                              jobjectArray buffers,
                                                              jlongArray metadata) {
    return guard<jlong>(env, 0, [&]() -> jlong {
        if (!buffers || !metadata) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        fqdb_store_results *out = nullptr;
        if (!check(env, fqdb_store_reader_acquire(ptr<fqdb_store_reader>(handle), &out)))
            return 0;
        std::unique_ptr<fqdb_store_results, decltype(&fqdb_store_results_close)> owner(
            out, fqdb_store_results_close);
        const auto count = fqdb_store_results_count(out);
        if (env->GetArrayLength(buffers) != static_cast<jsize>(count) ||
            env->GetArrayLength(metadata) != static_cast<jsize>(count * 2)) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        const auto rows = fqdb_store_results_data(out);
        std::vector<jlong> fields(count * 2);
        for (uint32_t i = 0; i < count; ++i) {
            fields[i * 2] = static_cast<jlong>(rows[i].version);
            fields[i * 2 + 1] = static_cast<jlong>(rows[i].expires_at_ms);
            if (rows[i].found) {
                auto buffer = env->NewDirectByteBuffer(const_cast<void *>(rows[i].value.data),
                                                       static_cast<jlong>(rows[i].value.size));
                if (!buffer)
                    return 0;
                env->SetObjectArrayElement(buffers, i, buffer);
                env->DeleteLocalRef(buffer);
                if (env->ExceptionCheck())
                    return 0;
            }
        }
        env->SetLongArrayRegion(metadata, 0, static_cast<jsize>(fields.size()), fields.data());
        if (env->ExceptionCheck())
            return 0;
        owner.release();
        return id(out);
    });
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nReaderRelease(JNIEnv *, jclass, jlong handle) {
    fqdb_store_reader_release(ptr<fqdb_store_reader>(handle));
}
JNIEXPORT jbyteArray JNICALL Java_io_fengqudb_Store_nGet(JNIEnv *env, jclass, jlong handle,
                                                         jbyteArray key, jint kind) {
    return guard<jbyteArray>(env, nullptr, [&]() -> jbyteArray {
        Input input(env, key);
        if (!input.valid())
            return nullptr;
        if (kind == 0 || kind == 2) {
            fqdb_store_shared_value shared{};
            auto status =
                kind == 0 ? fqdb_store_get_shared(ptr<fqdb_store>(handle), input.slice(), &shared)
                          : fqdb_store_snapshot_get_shared(ptr<fqdb_store_snapshot>(handle),
                                                           input.slice(), &shared);
            if (status == FQDB_NOT_FOUND)
                return nullptr;
            if (status != FQDB_LIMIT) {
                if (!check(env, status))
                    return nullptr;
                std::unique_ptr<fqdb_store_value, decltype(&fqdb_store_value_close)> owner(
                    shared.owner, fqdb_store_value_close);
                return copy(env, shared.row.value);
            }
        }
        fqdb_buffer buffer{};
        auto status =
            kind == 0
                ? fqdb_store_get(ptr<fqdb_store>(handle), input.slice(), &buffer)
                : (kind == 1
                       ? fqdb_store_txn_get(ptr<fqdb_store_txn>(handle), input.slice(), &buffer)
                       : (kind == 2 ? fqdb_store_snapshot_get(ptr<fqdb_store_snapshot>(handle),
                                                              input.slice(), &buffer)
                                    : FQDB_INVALID_ARGUMENT));
        if (status == FQDB_NOT_FOUND)
            return nullptr;
        if (!check(env, status))
            return nullptr;
        auto result = copy(env, {buffer.data, buffer.size});
        fqdb_buffer_free(&buffer);
        return result;
    });
}
JNIEXPORT jlongArray JNICALL Java_io_fengqudb_Store_nInfo(JNIEnv *env, jclass, jlong handle) {
    fqdb_store_info info{};
    if (!check(env, fqdb_store_get_info(ptr<fqdb_store>(handle), &info)))
        return nullptr;
    const uint64_t numbers[]{info.version,
                             info.durable_version,
                             info.file_bytes,
                             info.indexed_keys,
                             info.stored_keys,
                             info.stored_value_bytes,
                             info.delta_bytes,
                             info.cache_bytes,
                             info.pinned_bytes,
                             info.transaction_bytes,
                             info.recovered_transactions,
                             info.repaired_bytes};
    jlong values[12];
    for (size_t i = 0; i < 12; ++i)
        values[i] = static_cast<jlong>(numbers[i]);
    auto result = env->NewLongArray(12);
    if (result)
        env->SetLongArrayRegion(result, 0, 12, values);
    return result;
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nAdmin(JNIEnv *env, jclass, jlong handle,
                                                     jint operation) {
    check(env, operation == 0 ? fqdb_store_sync(ptr<fqdb_store>(handle))
                              : (operation == 1 ? fqdb_store_checkpoint(ptr<fqdb_store>(handle))
                                                : FQDB_INVALID_ARGUMENT));
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nBegin(JNIEnv *env, jclass, jlong handle,
                                                      jlong timeout) {
    if (timeout < 0) {
        check(env, FQDB_INVALID_ARGUMENT);
        return 0;
    }
    fqdb_store_txn *output = nullptr;
    if (!check(env, fqdb_store_txn_begin(ptr<fqdb_store>(handle), static_cast<uint64_t>(timeout),
                                         &output)))
        return 0;
    return id(output);
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nSnapshot(JNIEnv *env, jclass, jlong handle) {
    fqdb_store_snapshot *output = nullptr;
    if (!check(env, fqdb_store_snapshot_create(ptr<fqdb_store>(handle), &output)))
        return 0;
    return id(output);
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nEdit(JNIEnv *env, jclass, jlong handle,
                                                    jbyteArray key, jbyteArray bytes, jlong expires,
                                                    jint operation) {
    guard<int>(env, 0, [&] {
        Input input(env, key);
        if (!input.valid())
            return 0;
        if (expires < 0) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        if (operation == 0) {
            Input value(env, bytes);
            if (value.valid())
                check(env, fqdb_store_txn_put(ptr<fqdb_store_txn>(handle), input.slice(),
                                              value.slice(), static_cast<uint64_t>(expires)));
        } else
            check(env, operation == 1
                           ? fqdb_store_txn_delete(ptr<fqdb_store_txn>(handle), input.slice())
                           : (operation == 2 ? fqdb_store_txn_expire(ptr<fqdb_store_txn>(handle),
                                                                     input.slice(),
                                                                     static_cast<uint64_t>(expires))
                                             : FQDB_INVALID_ARGUMENT));
        return 0;
    });
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nPatch(JNIEnv *env, jclass, jlong handle,
                                                     jbyteArray key, jlong offset, jlong remove,
                                                     jbyteArray insert) {
    guard<int>(env, 0, [&] {
        Input k(env, key), value(env, insert);
        if (!k.valid() || !value.valid())
            return 0;
        if (offset < 0 || remove < 0) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        check(env, fqdb_store_txn_patch(ptr<fqdb_store_txn>(handle), k.slice(),
                                        static_cast<uint64_t>(offset),
                                        static_cast<uint64_t>(remove), value.slice()));
        return 0;
    });
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nSavepoint(JNIEnv *env, jclass, jlong handle,
                                                          jlong point, jint operation) {
    uint64_t output = 0;
    auto status =
        operation == 0
            ? fqdb_store_txn_savepoint(ptr<fqdb_store_txn>(handle), &output)
            : (operation == 1
                   ? fqdb_store_txn_rollback_to(ptr<fqdb_store_txn>(handle),
                                                static_cast<uint64_t>(point))
                   : (operation == 2 ? fqdb_store_txn_release(ptr<fqdb_store_txn>(handle),
                                                              static_cast<uint64_t>(point))
                                     : FQDB_INVALID_ARGUMENT));
    if (!check(env, status))
        return 0;
    return static_cast<jlong>(output);
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nCancel(JNIEnv *, jclass, jlong handle) {
    fqdb_store_txn_cancel(ptr<fqdb_store_txn>(handle));
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nCommit(JNIEnv *env, jclass, jlong handle,
                                                       jboolean force) {
    uint64_t version = 0;
    if (!check(env, fqdb_store_txn_commit(ptr<fqdb_store_txn>(handle), &version, force ? 1u : 0u)))
        return 0;
    return static_cast<jlong>(version);
}
JNIEXPORT jint JNICALL Java_io_fengqudb_Store_nAbiVersion(JNIEnv *, jclass) {
    return jint(fqdb_abi_version());
}
JNIEXPORT jlongArray JNICALL Java_io_fengqudb_Store_nWriteState(JNIEnv *env, jclass, jlong handle) {
    auto out = env->NewLongArray(4);
    if (!out)
        return nullptr;
    fqdb_store_write_state state{};
    if (!check(env, fqdb_store_write_state_get(ptr<fqdb_store>(handle), &state)))
        return nullptr;
    jlong v[]{jlong(state.version), jlong(state.durable_version), jlong(state.pending_bytes),
              jlong(state.last_status)};
    env->SetLongArrayRegion(out, 0, 4, v);
    return out;
}
JNIEXPORT jlongArray JNICALL Java_io_fengqudb_Store_nMutate(JNIEnv *env, jclass, jlong handle,
                                                            jint kind, jbyteArray key,
                                                            jbyteArray value, jlong offset,
                                                            jlong remove, jlong expires,
                                                            jboolean force) {
    return guard<jlongArray>(env, nullptr, [&]() -> jlongArray {
        auto out = env->NewLongArray(5);
        if (!out)
            return nullptr;
        if (offset < 0 || remove < 0 || expires < 0) {
            check(env, FQDB_INVALID_ARGUMENT);
            return nullptr;
        }
        Input k(env, key);
        if (!k.valid())
            return nullptr;
        std::unique_ptr<Input> v;
        if (kind != 1) {
            v = std::make_unique<Input>(env, value);
            if (!v->valid())
                return nullptr;
        }
        fqdb_store_atomic_info info{};
        fqdb_status status = FQDB_INVALID_ARGUMENT;
        auto db = ptr<fqdb_store>(handle);
        if (kind == 0)
            status = fqdb_store_put(db, k.slice(), v->slice(), uint64_t(expires), &info,
                                    force ? 1u : 0u);
        else if (kind == 1)
            status = fqdb_store_delete(db, k.slice(), &info, force ? 1u : 0u);
        else if (kind == 2)
            status = fqdb_store_patch(db, k.slice(), uint64_t(offset), uint64_t(remove), v->slice(),
                                      &info, force ? 1u : 0u);
        if (!check(env, status))
            return nullptr;
        jlong values[]{jlong(info.version), jlong(info.observed_version), jlong(info.value),
                       info.matched, info.changed};
        env->SetLongArrayRegion(out, 0, 5, values);
        return out;
    });
}
JNIEXPORT jlongArray JNICALL Java_io_fengqudb_Store_nWriteOptions(JNIEnv *env, jclass, jlong handle,
                                                                  jint action, jint mode,
                                                                  jlong period, jlong bytes) {
    auto output = env->NewLongArray(3);
    if (!output)
        return nullptr;
    auto store = ptr<fqdb_store>(handle);
    if (action == 1) {
        if (mode < 0 || mode > 1 || period < 0 || uint64_t(period) > UINT32_MAX || bytes < 0) {
            check(env, FQDB_INVALID_ARGUMENT);
            return nullptr;
        }
        fqdb_store_write_options p{sizeof(p), 1, uint32_t(mode), uint32_t(period), uint64_t(bytes)};
        if (!check(env, fqdb_store_write_configure(store, &p)))
            return nullptr;
    } else if (action == 2) {
        if (!check(env, fqdb_store_write_configure(store, nullptr)))
            return nullptr;
    } else if (action != 0) {
        check(env, FQDB_INVALID_ARGUMENT);
        return nullptr;
    }
    fqdb_store_write_options p{};
    if (!check(env, fqdb_store_write_options_get(store, &p)))
        return nullptr;
    jlong values[]{jlong(p.mode), jlong(p.period_ms), jlong(p.threshold_bytes)};
    env->SetLongArrayRegion(output, 0, 3, values);
    return output;
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nSubmitPut(JNIEnv *env, jclass, jlong handle,
                                                          jbyteArray key, jbyteArray value,
                                                          jlong expires, jboolean force) {
    return guard<jlong>(env, 0, [&]() -> jlong {
        Input k(env, key), v(env, value);
        if (!k.valid() || !v.valid())
            return 0;
        if (expires < 0) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        fqdb_store_commit *out = nullptr;
        if (!check(env, fqdb_store_put_submit(ptr<fqdb_store>(handle), k.slice(), v.slice(),
                                              uint64_t(expires), &out, force ? 1u : 0u)))
            return 0;
        return id(out);
    });
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nSubmitTxn(JNIEnv *env, jclass, jlong handle,
                                                          jboolean force) {
    fqdb_store_commit *out = nullptr;
    if (!check(env, fqdb_store_txn_submit(ptr<fqdb_store_txn>(handle), &out, force ? 1u : 0u)))
        return 0;
    return id(out);
}
JNIEXPORT jlongArray JNICALL Java_io_fengqudb_Store_nCommitResult(JNIEnv *env, jclass, jlong handle,
                                                                  jlong timeout, jboolean poll) {
    if (timeout < 0) {
        check(env, FQDB_INVALID_ARGUMENT);
        return nullptr;
    }
    auto output = env->NewLongArray(2);
    if (!output)
        return nullptr;
    fqdb_status completion;
    uint64_t version;
    auto status =
        poll ? fqdb_store_commit_poll(ptr<fqdb_store_commit>(handle), &completion, &version)
             : fqdb_store_commit_wait(ptr<fqdb_store_commit>(handle), uint64_t(timeout),
                                      &completion, &version);
    if (poll && status == FQDB_NOT_MODIFIED)
        return nullptr;
    if (!check(env, status))
        return nullptr;
    jlong values[]{completion, jlong(version)};
    env->SetLongArrayRegion(output, 0, 2, values);
    return output;
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nCommitCancel(JNIEnv *, jclass, jlong handle) {
    fqdb_store_commit_cancel(ptr<fqdb_store_commit>(handle));
}
JNIEXPORT jlong JNICALL Java_io_fengqudb_Store_nCursor(JNIEnv *env, jclass, jlong handle,
                                                       jbyteArray begin, jbyteArray end,
                                                       jboolean reverse, jboolean transaction) {
    return guard<jlong>(env, 0, [&]() -> jlong {
        Input low(env, begin), high(env, end);
        if (!low.valid() || !high.valid())
            return 0;
        fqdb_store_cursor *output = nullptr;
        auto status = transaction
                          ? fqdb_store_txn_cursor_open(ptr<fqdb_store_txn>(handle), low.slice(),
                                                       high.slice(), reverse ? 1u : 0u, &output)
                          : fqdb_store_cursor_open(ptr<fqdb_store_snapshot>(handle), low.slice(),
                                                   high.slice(), reverse ? 1u : 0u, &output);
        if (!check(env, status))
            return 0;
        return id(output);
    });
}
JNIEXPORT jobject JNICALL Java_io_fengqudb_Store_nNext(JNIEnv *env, jclass, jlong handle) {
    fqdb_store_row row{};
    auto status = fqdb_store_cursor_next(ptr<fqdb_store_cursor>(handle), &row);
    if (status == FQDB_END || !check(env, status))
        return nullptr;
    auto type = env->FindClass("io/fengqudb/Store$Row");
    if (!type)
        return nullptr;
    auto ctor = env->GetMethodID(type, "<init>", "([B[BJJ)V");
    if (!ctor) {
        env->DeleteLocalRef(type);
        return nullptr;
    }
    auto key = copy(env, row.key), value = env->ExceptionCheck() ? nullptr : copy(env, row.value);
    jobject result = nullptr;
    if (key && value && !env->ExceptionCheck())
        result = env->NewObject(type, ctor, key, value, static_cast<jlong>(row.version),
                                static_cast<jlong>(row.expires_at_ms));
    if (key)
        env->DeleteLocalRef(key);
    if (value)
        env->DeleteLocalRef(value);
    env->DeleteLocalRef(type);
    return result;
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nMaintenance(JNIEnv *env, jclass, jlong handle,
                                                           jbyteArray destination, jint operation,
                                                           jlongArray limits, jlongArray output) {
    guard<int>(env, 0, [&] {
        if (!output || (env->GetArrayLength(output) != 8 && env->GetArrayLength(output) != 11)) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        fqdb_store_maintenance_options options{};
        fqdb_store_maintenance_io io{};
        if (!maintenance_options(env, limits, options,
                                 env->GetArrayLength(output) == 11 ? &io : nullptr))
            return 0;
        fqdb_store_maintenance_info info{};
        fqdb_status status = FQDB_INVALID_ARGUMENT;
        if (operation == 0)
            status = fqdb_store_compact(ptr<fqdb_store>(handle), &options, &info);
        else if (operation == 2)
            status = fqdb_store_verify(ptr<fqdb_store>(handle), &options, &info);
        else if (operation == 1) {
            std::string path;
            if (!directory(env, destination, path))
                return 0;
            status = fqdb_store_backup(ptr<fqdb_store>(handle), path.c_str(), &options, &info);
        }
        maintenance_io(env, io, output);
        if (!check(env, status))
            return 0;
        maintenance_info(env, info, output);
        return 0;
    });
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nRestore(JNIEnv *env, jclass, jbyteArray source,
                                                       jbyteArray destination, jlongArray limits,
                                                       jlongArray output) {
    guard<int>(env, 0, [&] {
        if (!output || (env->GetArrayLength(output) != 8 && env->GetArrayLength(output) != 11)) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        fqdb_store_maintenance_options options{};
        fqdb_store_maintenance_io io{};
        if (!maintenance_options(env, limits, options,
                                 env->GetArrayLength(output) == 11 ? &io : nullptr))
            return 0;
        std::string from, to;
        if (!directory(env, source, from) || !directory(env, destination, to))
            return 0;
        fqdb_store_maintenance_info info{};
        auto status = fqdb_store_restore(from.c_str(), to.c_str(), &options, &info);
        maintenance_io(env, io, output);
        if (!check(env, status))
            return 0;
        maintenance_info(env, info, output);
        return 0;
    });
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nPurgeExpired(JNIEnv *env, jclass, jlong handle,
                                                            jlongArray limits, jlongArray output) {
    guard<int>(env, 0, [&] {
        if (!limits || env->GetArrayLength(limits) != 3 || !output ||
            env->GetArrayLength(output) != 6) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        jlong values[3]{};
        env->GetLongArrayRegion(limits, 0, 3, values);
        if (env->ExceptionCheck())
            return 0;
        if (values[0] < 1 || values[0] > 65536 || values[1] < 0 || values[2] < 0) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        fqdb_store_expiry_options options{sizeof(options),
                                          1,
                                          static_cast<uint32_t>(values[0]),
                                          0,
                                          static_cast<uint64_t>(values[1]),
                                          static_cast<uint64_t>(values[2])};
        fqdb_store_expiry_info info{};
        if (!check(env, fqdb_store_purge_expired(ptr<fqdb_store>(handle), &options, &info)))
            return 0;
        const jlong result[]{
            static_cast<jlong>(info.version),       static_cast<jlong>(info.cutoff_ms),
            static_cast<jlong>(info.removed_keys),  static_cast<jlong>(info.removed_value_bytes),
            static_cast<jlong>(info.visited_pages), static_cast<jlong>(info.more)};
        env->SetLongArrayRegion(output, 0, 6, result);
        return 0;
    });
}
JNIEXPORT jbyteArray JNICALL Java_io_fengqudb_Store_nHashGet(JNIEnv *env, jclass, jlong handle,
                                                             jbyteArray scope, jbyteArray key) {
    return guard<jbyteArray>(env, nullptr, [&]() -> jbyteArray {
        Input s(env, scope), k(env, key);
        if (!s.valid() || !k.valid())
            return nullptr;
        fqdb_store_value *value = nullptr;
        auto status =
            fqdb_store_hash_get_view(ptr<fqdb_store>(handle), s.slice(), k.slice(), &value);
        if (status == FQDB_NOT_FOUND)
            return nullptr;
        if (!check(env, status))
            return nullptr;
        std::unique_ptr<fqdb_store_value, decltype(&fqdb_store_value_close)> owner(
            value, fqdb_store_value_close);
        return copy(env, fqdb_store_value_data(value));
    });
}
// Read-only SELECT/EXPLAIN on a fresh snapshot of the Store handle.
JNIEXPORT jbyteArray JNICALL Java_io_fengqudb_Store_nSelect(JNIEnv *env, jclass, jlong handle,
                                                            jbyteArray request) {
    return guard<jbyteArray>(env, nullptr, [&]() -> jbyteArray {
        Input input(env, request);
        if (!input.valid())
            return nullptr;
        fqdb_buffer output{};
        auto status = fqdb_store_select(ptr<fqdb_store>(handle), input.slice(), &output);
        struct Free {
            fqdb_buffer *p;
            ~Free() {
                fqdb_buffer_free(p);
            }
        } free{&output};
        if (!check(env, status))
            return nullptr;
        return copy(env, {output.data, output.size});
    });
}
JNIEXPORT jbyteArray JNICALL Java_io_fengqudb_Store_nExecute(JNIEnv *env, jclass, jlong handle,
                                                             jbyteArray request) {
    return guard<jbyteArray>(env, nullptr, [&]() -> jbyteArray {
        Input input(env, request);
        if (!input.valid())
            return nullptr;
        fqdb_buffer output{};
        auto status = fqdb_store_txn_execute(ptr<fqdb_store_txn>(handle), input.slice(), &output);
        struct Free {
            fqdb_buffer *p;
            ~Free() {
                fqdb_buffer_free(p);
            }
        } free{&output};
        if (!check(env, status))
            return nullptr;
        return copy(env, {output.data, output.size});
    });
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nFeatureAdminEx(JNIEnv *env, jclass, jlong handle,
                                                              jint operation, jobjectArray texts,
                                                              jlongArray numbers, jlongArray budget,
                                                              jlongArray output) {
    guard<int>(env, 0, [&] {
        if (!texts || !numbers || !output ||
            (env->GetArrayLength(output) != 8 && env->GetArrayLength(output) != 11)) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        auto count = env->GetArrayLength(texts), size = env->GetArrayLength(numbers);
        if (count > 1026 || size > 16) {
            check(env, FQDB_INVALID_ARGUMENT);
            return 0;
        }
        std::vector<std::string> strings;
        strings.reserve(static_cast<size_t>(count));
        for (jsize i = 0; i < count; ++i) {
            auto array = static_cast<jbyteArray>(env->GetObjectArrayElement(texts, i));
            if (env->ExceptionCheck())
                return 0;
            {
                Input input(env, array);
                if (!input.valid()) {
                    if (array)
                        env->DeleteLocalRef(array);
                    return 0;
                }
                auto value = input.slice();
                strings.emplace_back(value.size ? static_cast<const char *>(value.data) : "",
                                     static_cast<size_t>(value.size));
            }
            env->DeleteLocalRef(array);
        }
        jlong values[16]{};
        env->GetLongArrayRegion(numbers, 0, size, values);
        if (env->ExceptionCheck())
            return 0;
        for (jsize i = 0; i < size; ++i)
            if (values[i] < 0 && !(i == 0 && (operation == 2 || operation == 3))) {
                check(env, FQDB_INVALID_ARGUMENT);
                return 0;
            }
        if (operation >= 2 && operation <= 4)
            for (const auto &text : strings)
                if (text.empty() || text.find('\0') != std::string::npos) {
                    check(env, FQDB_INVALID_ARGUMENT);
                    return 0;
                }
        auto slice = [&](size_t i) { return fqdb_slice{strings[i].data(), strings[i].size()}; };
        fqdb_store_maintenance_options settings{sizeof(settings), 1, 8, 0,      0,
                                                1048576,          0, 0, nullptr};
        fqdb_store_maintenance_io io{};
        if (budget && !maintenance_options(env, budget, settings,
                                           env->GetArrayLength(output) == 11 ? &io : nullptr))
            return 0;
        fqdb_store_maintenance_info info{};
        fqdb_status status = FQDB_INVALID_ARGUMENT;
        auto store = ptr<fqdb_store>(handle);
        if (operation == 0 && count == 2 && size == 4 && values[0] <= UINT32_MAX &&
            values[1] <= UINT32_MAX && values[2] <= UINT32_MAX) {
            if (!budget)
                settings.timeout_ms = static_cast<uint64_t>(values[3]);
            status =
                fqdb_store_index_define(store, slice(0), slice(1), static_cast<uint32_t>(values[0]),
                                        static_cast<uint32_t>(values[1]),
                                        static_cast<uint32_t>(values[2]), &settings, &info);
        } else if (operation == 7 && count == 2 && size >= 3 && size <= 10 && values[0] <= 3) {
            std::array<fqdb_store_index_field, 8> fields{};
            for (jsize i = 2; i < size; ++i) {
                auto bits = static_cast<uint64_t>(values[i]);
                fields[static_cast<size_t>(i - 2)] = {static_cast<uint32_t>(bits >> 32),
                                                      static_cast<uint32_t>(bits)};
            }
            if (!budget)
                settings.timeout_ms = static_cast<uint64_t>(values[1]);
            status = fqdb_store_index_define_compound(
                store, slice(0), slice(1), fields.data(), static_cast<uint32_t>(size - 2),
                static_cast<uint32_t>(values[0]), &settings, &info);
        } else if (operation == 1 && count == 1 && size == 1) {
            if (!budget)
                settings.timeout_ms = static_cast<uint64_t>(values[0]);
            status = fqdb_store_index_drop(store, slice(0), &settings, &info);
        } else if (operation == 2 && count == 1 && size == 2) {
            if (!budget)
                settings.timeout_ms = static_cast<uint64_t>(values[1]);
            status = fqdb_store_backup_incremental(
                store, strings[0].c_str(), static_cast<uint64_t>(values[0]), &settings, &info);
        } else if (operation == 3 && count >= 2 && size == 3) {
            if (!budget)
                settings.timeout_ms = static_cast<uint64_t>(values[2]);
            std::vector<const char *> paths;
            paths.reserve(static_cast<size_t>(count - 2));
            for (jsize i = 2; i < count; ++i)
                paths.push_back(strings[static_cast<size_t>(i)].c_str());
            status = fqdb_store_recover_chain(strings[0].c_str(), paths.data(),
                                              static_cast<uint32_t>(paths.size()),
                                              strings[1].c_str(), static_cast<uint64_t>(values[0]),
                                              static_cast<uint64_t>(values[1]), &settings, &info);
        } else if (operation == 5 && count == 0 && (size == 9 || size == 11)) {
            for (unsigned i = 0; i < 6; ++i)
                if (static_cast<uint64_t>(values[i]) > UINT32_MAX) {
                    check(env, FQDB_INVALID_ARGUMENT);
                    return 0;
                }
            fqdb_store_runtime_options options{sizeof(options),
                                               values[9] || values[10] ? 2u : 1u,
                                               static_cast<uint32_t>(values[0]),
                                               static_cast<uint32_t>(values[1]),
                                               static_cast<uint32_t>(values[2]),
                                               static_cast<uint32_t>(values[3]),
                                               static_cast<uint32_t>(values[4]),
                                               static_cast<uint32_t>(values[5]),
                                               static_cast<uint64_t>(values[6]),
                                               static_cast<uint64_t>(values[7]),
                                               static_cast<uint64_t>(values[8]),
                                               static_cast<uint64_t>(values[9]),
                                               static_cast<uint64_t>(values[10])};
            status = fqdb_store_configure(store, &options);
        } else if (operation == 6 && count == 0 && size == 0) {
            fqdb_store_runtime_info runtime{};
            status = fqdb_store_runtime_info_get(store, &runtime);
            if (!check(env, status))
                return 0;
            const jlong result[]{static_cast<jlong>(runtime.running),
                                 static_cast<jlong>(runtime.last_status),
                                 static_cast<jlong>(runtime.expiry_batches),
                                 static_cast<jlong>(runtime.compactions),
                                 static_cast<jlong>(runtime.group_flushes),
                                 static_cast<jlong>(runtime.group_transactions),
                                 0,
                                 0};
            env->SetLongArrayRegion(output, 0, 8, result);
            return 0;
        }
        if (operation == 8 && count == 0 && size == 0) {
            fqdb_store_history_info history{};
            status = fqdb_store_history_info_get(store, &history);
            if (!check(env, status))
                return 0;
            jlong history_values[]{static_cast<jlong>(history.version),
                                   static_cast<jlong>(history.floor),
                                   static_cast<jlong>(history.oldest_pin),
                                   static_cast<jlong>(history.active_pins),
                                   static_cast<jlong>(history.total_pins),
                                   static_cast<jlong>(history.retained_generations),
                                   static_cast<jlong>(history.retained_file_bytes),
                                   0};
            env->SetLongArrayRegion(output, 0, 8, history_values);
            return 0;
        }
        maintenance_io(env, io, output);
        if (check(env, status))
            maintenance_info(env, info, output);
        return 0;
    });
}
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nCancelMaintenance(JNIEnv *, jclass, jlong handle) {
    fqdb_store_cancel_maintenance(ptr<fqdb_store>(handle));
}
// Keep the previous private JNI entry callable by already compiled jars.
JNIEXPORT void JNICALL Java_io_fengqudb_Store_nFeatureAdmin(JNIEnv *env, jclass type, jlong handle,
                                                            jint operation, jobjectArray texts,
                                                            jlongArray numbers, jlongArray output) {
    Java_io_fengqudb_Store_nFeatureAdminEx(env, type, handle, operation, texts, numbers, nullptr,
                                           output);
}
}

extern "C" JNIEXPORT jstring JNICALL Java_io_fengqudb_Store_nStatusMessage(JNIEnv *env, jclass,
                                                                           jint status) {
    return env->NewStringUTF(fqdb_status_message(status));
}
