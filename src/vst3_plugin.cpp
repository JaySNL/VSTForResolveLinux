// One VST3 plugin per Fairlight effect.
//
// The ABI comes from travesty (third_party/dpf/distrho/src/travesty), DPF's clean-room C header
// set for VST3. The Steinberg SDK is GPL3-or-commercial and would decide this project's licence
// for it; travesty is ISC and is plain C structs, which is the whole reason it is usable here.
//
// A VST3 "plugin" is three objects, and mixing them up is the usual way this goes wrong:
//   - the component      (v3_component)        - buses, state, activation
//   - the audio processor (v3_audio_processor) - the same object, queried for another interface
//   - the edit controller (v3_edit_controller) - parameters and the editor window
// The controller is often a separate class with its own id, which is why get_controller_class_id
// is asked before falling back to querying the component itself.
//
// yabridge presents a Windows VST3 as a native Linux bundle with the same layout, so this host
// reaches the Windows plugins on this machine as well as the native ones.
#include "vst3_plugin.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <dirent.h>
#include <poll.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "host_thread.h"
#include "plugin_window.h"

#include "base.h"
#include "factory.h"
#include "bstream.h"
#include "component.h"
#include "audio_processor.h"
#include "edit_controller.h"
#include "message.h"
#include "view.h"
#include "host.h"

namespace {

void (*g_logger)(const char*) = nullptr;

void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

void Log(const char* format, ...)
{
    if (g_logger == nullptr) {
        return;
    }
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_logger(line);
}

// --- calling the objects ------------------------------------------------------------------------
//
// Every VST3 object is a pointer to a pointer to its vtable, and the vtable always starts with
// funknown. The _cpp structs in travesty describe the whole vtable, so casting to those and
// naming the sub-struct is unambiguous - unlike v3_cpp_obj, which has to guess the offset from
// the type and needs a specialisation per interface.

template <class T>
v3_funknown* Unknown(T** object)
{
    return static_cast<v3_funknown*>(static_cast<void*>(*object));
}

template <class T>
bool Query(T** object, const v3_tuid iid, void** out)
{
    return object != nullptr && Unknown(object)->query_interface(object, iid, out) == V3_OK &&
           *out != nullptr;
}

template <class T>
void Release(T**& object)
{
    if (object != nullptr) {
        Unknown(object)->unref(object);
        object = nullptr;
    }
}

// --- the host we present ------------------------------------------------------------------------
//
// A plugin that finds no host context refuses to initialise, and one that finds no component
// handler often refuses to build its editor. Both are answered, and neither does anything: this
// bridge sends no parameter changes, so there is nothing for an edit gesture to report.

v3_result V3_API HostQueryInterface(void* self, const v3_tuid iid, void** obj)
{
    if (std::memcmp(iid, v3_funknown_iid, sizeof(v3_tuid)) == 0 ||
        std::memcmp(iid, v3_host_application_iid, sizeof(v3_tuid)) == 0) {
        *obj = self;
        return V3_OK;
    }
    *obj = nullptr;
    return V3_NO_INTERFACE;
}

uint32_t V3_API HostRef(void*) { return 1; }
uint32_t V3_API HostUnref(void*) { return 1; }

v3_result V3_API HostGetName(void*, v3_str_128 name)
{
    static const char text[] = "Jay's Desk FX bridge";
    for (size_t index = 0; index < sizeof(text); ++index) {
        name[index] = static_cast<int16_t>(text[index]);
    }
    return V3_OK;
}

v3_result V3_API HostCreateInstance(void*, v3_tuid, v3_tuid, void** obj)
{
    *obj = nullptr;
    return V3_NOT_IMPLEMENTED;
}

const v3_host_application_cpp g_host_vtable = {
    {HostQueryInterface, HostRef, HostUnref},
    {HostGetName, HostCreateInstance},
};
const v3_host_application_cpp* g_host_vtable_pointer = &g_host_vtable;
v3_funknown** const g_host_context =
    reinterpret_cast<v3_funknown**>(const_cast<v3_host_application_cpp**>(&g_host_vtable_pointer));

v3_result V3_API HandlerQueryInterface(void* self, const v3_tuid iid, void** obj)
{
    if (std::memcmp(iid, v3_funknown_iid, sizeof(v3_tuid)) == 0 ||
        std::memcmp(iid, v3_component_handler_iid, sizeof(v3_tuid)) == 0) {
        *obj = self;
        return V3_OK;
    }
    *obj = nullptr;
    return V3_NO_INTERFACE;
}

// The editor talks to the host, and the host is the only thing that reaches the processor.
//
// VST3 splits a plugin in two. The controller draws the editor; the component does the audio; they
// are not required to share so much as a variable. When the user drags a band, the controller
// calls performEdit on the host and stops there. The host has to carry that value to the processor
// as a parameter change on the next process call - nothing else will.
//
// This used to be `return V3_OK` and nothing else, and one global handler shared by every effect,
// which could not have said which plugin was speaking even if it had wanted to. Measured on
// 2026-08-29: two F6-RTA editors set to visibly different curves, and both state files stayed
// byte-identical to the moment they were created. The processor never heard a thing.
class Vst3Plugin;
void PluginParameterEdited(Vst3Plugin* plugin, v3_param_id id, double value);

// One per effect, carrying the back-pointer the global handler never had.
struct EditorHandler {
    const v3_component_handler_cpp* vtable;
    Vst3Plugin* owner;
};

v3_result V3_API HandlerBeginEdit(void*, v3_param_id) { return V3_OK; }

v3_result V3_API HandlerPerformEdit(void* self, v3_param_id id, double value)
{
    auto* const handler = static_cast<EditorHandler*>(self);
    if (handler != nullptr && handler->owner != nullptr) {
        PluginParameterEdited(handler->owner, id, value);
    }
    return V3_OK;
}

v3_result V3_API HandlerEndEdit(void*, v3_param_id) { return V3_OK; }
v3_result V3_API HandlerRestart(void*, int32_t) { return V3_OK; }

const v3_component_handler_cpp g_handler_vtable = {
    {HandlerQueryInterface, HostRef, HostUnref},
    {HandlerBeginEdit, HandlerPerformEdit, HandlerEndEdit, HandlerRestart},
};

// The parameter changes handed to one process call.
//
// Both objects live inside the effect and are refilled per block, so the audio thread allocates
// nothing. A queue carries a single point at offset zero: this is a knob being turned now, not an
// automation curve being played back.
constexpr int32_t kMaxQueuedParams = 128;

struct ParamQueue {
    const v3_param_value_queue_cpp* vtable;
    v3_param_id id;
    double value;
};

struct ParamChanges {
    const v3_param_changes_cpp* vtable;
    ParamQueue* queues;
    int32_t count;
};

v3_result V3_API QueueQueryInterface(void* self, const v3_tuid iid, void** obj)
{
    if (std::memcmp(iid, v3_funknown_iid, sizeof(v3_tuid)) == 0 ||
        std::memcmp(iid, v3_param_value_queue_iid, sizeof(v3_tuid)) == 0) {
        *obj = self;
        return V3_OK;
    }
    *obj = nullptr;
    return V3_NO_INTERFACE;
}

v3_param_id V3_API QueueGetParamId(void* self)
{
    return static_cast<ParamQueue*>(self)->id;
}

int32_t V3_API QueueGetPointCount(void*) { return 1; }

v3_result V3_API QueueGetPoint(void* self, int32_t index, int32_t* offset, double* value)
{
    if (index != 0 || offset == nullptr || value == nullptr) {
        return V3_INVALID_ARG;
    }
    *offset = 0;
    *value = static_cast<ParamQueue*>(self)->value;
    return V3_OK;
}

v3_result V3_API QueueAddPoint(void*, int32_t, double, int32_t*) { return V3_NOT_IMPLEMENTED; }

const v3_param_value_queue_cpp g_queue_vtable = {
    {QueueQueryInterface, HostRef, HostUnref},
    {QueueGetParamId, QueueGetPointCount, QueueGetPoint, QueueAddPoint},
};

v3_result V3_API ChangesQueryInterface(void* self, const v3_tuid iid, void** obj)
{
    if (std::memcmp(iid, v3_funknown_iid, sizeof(v3_tuid)) == 0 ||
        std::memcmp(iid, v3_param_changes_iid, sizeof(v3_tuid)) == 0) {
        *obj = self;
        return V3_OK;
    }
    *obj = nullptr;
    return V3_NO_INTERFACE;
}

int32_t V3_API ChangesGetParamCount(void* self)
{
    return static_cast<ParamChanges*>(self)->count;
}

v3_param_value_queue** V3_API ChangesGetParamData(void* self, int32_t index)
{
    auto* const changes = static_cast<ParamChanges*>(self);
    if (index < 0 || index >= changes->count) {
        return nullptr;
    }
    return reinterpret_cast<v3_param_value_queue**>(&changes->queues[index]);
}

// Input changes are ours to fill and the plugin's to read.
v3_param_value_queue** V3_API ChangesAddParamData(void*, const v3_param_id*, int32_t*)
{
    return nullptr;
}

const v3_param_changes_cpp g_changes_vtable = {
    {ChangesQueryInterface, HostRef, HostUnref},
    {ChangesGetParamCount, ChangesGetParamData, ChangesAddParamData},
};

// --- the bundle ----------------------------------------------------------------------------------

bool IsDirectory(const std::string& path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// Name.vst3/Contents/x86_64-linux/Name.so. The scanner records the bundle directory, because that
// is the thing a user recognises; the binary inside is found here.
std::string BinaryInsideBundle(const std::string& bundle)
{
    if (!IsDirectory(bundle)) {
        return bundle;  // the flat, single-file variant
    }
    const std::string folder = bundle + "/Contents/x86_64-linux";
    DIR* const directory = opendir(folder.c_str());
    if (directory == nullptr) {
        return std::string();
    }
    std::string found;
    while (const dirent* const item = readdir(directory)) {
        const std::string name = item->d_name;
        if (name.size() > 3 && name.compare(name.size() - 3, 3, ".so") == 0) {
            found = folder + "/" + name;
            break;
        }
    }
    closedir(directory);
    return found;
}

constexpr v3_speaker_arrangement kArrangementMono = 1;    // front left only
constexpr v3_speaker_arrangement kArrangementStereo = 3;  // front left and front right


// --- the run loop a Linux VST3 editor needs -------------------------------------------------------
//
// VST3 on Linux puts the event loop in the host's hands. A plugin's editor hands the host the file
// descriptor of its X11 connection plus its timers, and the host calls back. There is no polite
// degradation if the host does not: DPF's attached() reads
//
//     DISTRHO_SAFE_ASSERT_RETURN(view->frame != nullptr, V3_INVALID_ARG);
//     v3_cpp_obj_query_interface(view->frame, v3_run_loop_iid, &runloop);
//     DISTRHO_SAFE_ASSERT_RETURN(runloop != nullptr, V3_INVALID_ARG);
//
// so a host that skips set_frame gets a refusal and an empty window. That is exactly what this
// bridge showed on 2026-08-25: "attached() refused", and a black panel with a working audio path
// behind it.
//
// The object carries two vtable pointers, the way a C++ class with two bases does. The view is
// handed the frame face; the frame answers query_interface(IRunLoop) with the other face. Each
// face holds a back pointer, so recovering the object never needs offsetof.

class EditorRunLoop;

extern const v3_plugin_frame_cpp kFrameVtable;
extern const v3_run_loop_cpp kLoopVtable;

class EditorRunLoop {
public:
    struct FrameFace {
        const v3_plugin_frame_cpp* vtable;
        EditorRunLoop* owner;
    };
    struct LoopFace {
        const v3_run_loop_cpp* vtable;
        EditorRunLoop* owner;
    };

    EditorRunLoop()
    {
        frame_face_.vtable = &kFrameVtable;
        frame_face_.owner = this;
        loop_face_.vtable = &kLoopVtable;
        loop_face_.owner = this;
    }

    v3_plugin_frame** Frame() { return reinterpret_cast<v3_plugin_frame**>(&frame_face_); }

    // Called on the host main thread with HostMainLock() held, like every other loader's tick.
    void Service()
    {
        // Copy before calling out: a handler may unregister itself from inside its own callback.
        const std::vector<Handler> handlers = handlers_;
        if (!handlers.empty()) {
            std::vector<pollfd> slots;
            slots.reserve(handlers.size());
            for (const Handler& entry : handlers) {
                pollfd slot{};
                slot.fd = entry.fd;
                slot.events = POLLIN;
                slots.push_back(slot);
            }
            if (poll(slots.data(), slots.size(), 0) > 0) {
                for (size_t index = 0; index < handlers.size(); ++index) {
                    if (slots[index].revents == 0) {
                        continue;
                    }
                    auto* const vt =
                        *reinterpret_cast<v3_event_handler_cpp**>(handlers[index].handler);
                    vt->handler.on_fd_is_set(handlers[index].handler, handlers[index].fd);
                }
            }
        }

        const auto now = std::chrono::steady_clock::now();
        const std::vector<Timer> due = timers_;
        for (const Timer& entry : due) {
            if (entry.due > now) {
                continue;
            }
            auto* const vt = *reinterpret_cast<v3_timer_handler_cpp**>(entry.handler);
            vt->timer.on_timer(entry.handler);
        }
        for (Timer& entry : timers_) {
            if (entry.due <= now) {
                entry.due = now + entry.period;
            }
        }
    }

    // The plugin is expected to unregister on removed(). This is the belt for the times it does
    // not: a stale handler is a call into a freed editor on the next tick.
    void Forget()
    {
        handlers_.clear();
        timers_.clear();
    }

    static v3_result V3_API FrameQueryInterface(void* self, const v3_tuid iid, void** obj)
    {
        EditorRunLoop* const loop = static_cast<FrameFace*>(self)->owner;
        if (std::memcmp(iid, v3_funknown_iid, sizeof(v3_tuid)) == 0 ||
            std::memcmp(iid, v3_plugin_frame_iid, sizeof(v3_tuid)) == 0) {
            *obj = self;
            return V3_OK;
        }
        if (std::memcmp(iid, v3_run_loop_iid, sizeof(v3_tuid)) == 0) {
            *obj = &loop->loop_face_;
            return V3_OK;
        }
        *obj = nullptr;
        return V3_NO_INTERFACE;
    }

    static v3_result V3_API LoopQueryInterface(void* self, const v3_tuid iid, void** obj)
    {
        EditorRunLoop* const loop = static_cast<LoopFace*>(self)->owner;
        if (std::memcmp(iid, v3_funknown_iid, sizeof(v3_tuid)) == 0 ||
            std::memcmp(iid, v3_run_loop_iid, sizeof(v3_tuid)) == 0) {
            *obj = self;
            return V3_OK;
        }
        if (std::memcmp(iid, v3_plugin_frame_iid, sizeof(v3_tuid)) == 0) {
            *obj = &loop->frame_face_;
            return V3_OK;
        }
        *obj = nullptr;
        return V3_NO_INTERFACE;
    }

    // The editor asks to grow or shrink. The host owns the parent window, so the host resizes it
    // and then tells the view the size it got.
    static v3_result V3_API FrameResizeView(void* self, v3_plugin_view** view, v3_view_rect* rect)
    {
        EditorRunLoop* const loop = static_cast<FrameFace*>(self)->owner;
        if (view == nullptr || rect == nullptr) {
            return V3_INVALID_ARG;
        }
        const int32_t width = rect->right - rect->left;
        const int32_t height = rect->bottom - rect->top;
        if (width <= 0 || height <= 0) {
            return V3_INVALID_ARG;
        }
        if (loop->window != nullptr) {
            PluginWindowResize(loop->window, static_cast<unsigned int>(width),
                               static_cast<unsigned int>(height));
        }
        auto* const vt = *reinterpret_cast<v3_plugin_view_cpp**>(view);
        vt->view.on_size(view, rect);
        return V3_OK;
    }

    static v3_result V3_API LoopRegisterEventHandler(void* self, v3_event_handler** handler, int fd)
    {
        EditorRunLoop* const loop = static_cast<LoopFace*>(self)->owner;
        if (handler == nullptr || fd < 0) {
            return V3_INVALID_ARG;
        }
        loop->handlers_.push_back(Handler{handler, fd});
        return V3_OK;
    }

    static v3_result V3_API LoopUnregisterEventHandler(void* self, v3_event_handler** handler)
    {
        EditorRunLoop* const loop = static_cast<LoopFace*>(self)->owner;
        for (size_t index = 0; index < loop->handlers_.size(); ++index) {
            if (loop->handlers_[index].handler == handler) {
                loop->handlers_.erase(loop->handlers_.begin() + static_cast<long>(index));
                return V3_OK;
            }
        }
        return V3_INVALID_ARG;
    }

    static v3_result V3_API LoopRegisterTimer(void* self, v3_timer_handler** handler, uint64_t ms)
    {
        EditorRunLoop* const loop = static_cast<LoopFace*>(self)->owner;
        if (handler == nullptr) {
            return V3_INVALID_ARG;
        }
        // A zero period means "as often as you can", not "spin".
        const auto period = std::chrono::milliseconds(ms == 0 ? 1 : ms);
        loop->timers_.push_back(Timer{handler, period, std::chrono::steady_clock::now() + period});
        return V3_OK;
    }

    static v3_result V3_API LoopUnregisterTimer(void* self, v3_timer_handler** handler)
    {
        EditorRunLoop* const loop = static_cast<LoopFace*>(self)->owner;
        for (size_t index = 0; index < loop->timers_.size(); ++index) {
            if (loop->timers_[index].handler == handler) {
                loop->timers_.erase(loop->timers_.begin() + static_cast<long>(index));
                return V3_OK;
            }
        }
        return V3_INVALID_ARG;
    }

    PluginWindow* window = nullptr;

private:
    struct Handler {
        v3_event_handler** handler;
        int fd;
    };
    struct Timer {
        v3_timer_handler** handler;
        std::chrono::steady_clock::duration period;
        std::chrono::steady_clock::time_point due;
    };

    FrameFace frame_face_{};
    LoopFace loop_face_{};
    std::vector<Handler> handlers_;
    std::vector<Timer> timers_;
};

const v3_plugin_frame_cpp kFrameVtable = {
    {EditorRunLoop::FrameQueryInterface, HostRef, HostUnref},
    {EditorRunLoop::FrameResizeView},
};

const v3_run_loop_cpp kLoopVtable = {
    {EditorRunLoop::LoopQueryInterface, HostRef, HostUnref},
    {EditorRunLoop::LoopRegisterEventHandler, EditorRunLoop::LoopUnregisterEventHandler,
     EditorRunLoop::LoopRegisterTimer, EditorRunLoop::LoopUnregisterTimer},
};

// A v3_bstream over a byte vector.
//
// VST3 never hands its state back as a buffer. get_state writes into a stream the host supplies,
// so being the host means owning one. This is the whole of it: no file, no growth policy, no
// reference counting that means anything - the object lives for one call and dies on the stack.
struct MemoryStream {
    const v3_bstream_cpp* vtable;
    std::vector<uint8_t>* bytes;
    int64_t position;
};

v3_result V3_API StreamQueryInterface(void* self, const v3_tuid iid, void** obj)
{
    if (std::memcmp(iid, v3_funknown_iid, sizeof(v3_tuid)) == 0 ||
        std::memcmp(iid, v3_bstream_iid, sizeof(v3_tuid)) == 0) {
        *obj = self;
        return V3_OK;
    }
    *obj = nullptr;
    return V3_NO_INTERFACE;
}

v3_result V3_API StreamRead(void* self, void* buffer, int32_t num_bytes, int32_t* bytes_read)
{
    auto* const stream = static_cast<MemoryStream*>(self);
    if (stream == nullptr || stream->bytes == nullptr || buffer == nullptr || num_bytes < 0) {
        return V3_INVALID_ARG;
    }
    const int64_t left = static_cast<int64_t>(stream->bytes->size()) - stream->position;
    const int64_t take = left < num_bytes ? (left > 0 ? left : 0) : num_bytes;
    if (take > 0) {
        std::memcpy(buffer, stream->bytes->data() + stream->position, static_cast<size_t>(take));
        stream->position += take;
    }
    if (bytes_read != nullptr) {
        *bytes_read = static_cast<int32_t>(take);
    }
    return V3_OK;
}

v3_result V3_API StreamWrite(void* self, void* buffer, int32_t num_bytes, int32_t* bytes_written)
{
    auto* const stream = static_cast<MemoryStream*>(self);
    if (stream == nullptr || stream->bytes == nullptr || buffer == nullptr || num_bytes < 0) {
        return V3_INVALID_ARG;
    }
    const size_t at = static_cast<size_t>(stream->position);
    if (at + static_cast<size_t>(num_bytes) > stream->bytes->size()) {
        stream->bytes->resize(at + static_cast<size_t>(num_bytes));
    }
    std::memcpy(stream->bytes->data() + at, buffer, static_cast<size_t>(num_bytes));
    stream->position += num_bytes;
    if (bytes_written != nullptr) {
        *bytes_written = num_bytes;
    }
    return V3_OK;
}

v3_result V3_API StreamSeek(void* self, int64_t pos, int32_t mode, int64_t* result)
{
    auto* const stream = static_cast<MemoryStream*>(self);
    if (stream == nullptr || stream->bytes == nullptr) {
        return V3_INVALID_ARG;
    }
    const int64_t end = static_cast<int64_t>(stream->bytes->size());
    int64_t target = pos;
    if (mode == V3_SEEK_CUR) {
        target = stream->position + pos;
    } else if (mode == V3_SEEK_END) {
        target = end + pos;
    }
    if (target < 0) {
        target = 0;
    }
    if (target > end) {
        target = end;
    }
    stream->position = target;
    if (result != nullptr) {
        *result = target;
    }
    return V3_OK;
}

v3_result V3_API StreamTell(void* self, int64_t* pos)
{
    auto* const stream = static_cast<MemoryStream*>(self);
    if (stream == nullptr || pos == nullptr) {
        return V3_INVALID_ARG;
    }
    *pos = stream->position;
    return V3_OK;
}

const v3_bstream_cpp g_stream_vtable = {
    {StreamQueryInterface, HostRef, HostUnref},
    {StreamRead, StreamWrite, StreamSeek, StreamTell},
};

// Tells two effects of the same plugin apart in the log.
int g_plugin_serial = 0;

class Vst3Plugin final : public HostedPlugin, public HostMainClient {
public:
    ~Vst3Plugin() override
    {
        // Deregister first, and under the host lock: a tick that lands after the view is released
        // calls into a freed editor. Same order as the VST2 loader, for the same reason.
        {
            std::lock_guard<std::mutex> held(HostMainLock());
            HostMainUnregister(this);
        }
        run_loop_.Forget();
        if (view_ != nullptr) {
            auto* const vt = *reinterpret_cast<v3_plugin_view_cpp**>(view_);
            vt->view.removed(view_);
            Release(view_);
        }
        if (window_ != nullptr) {
            PluginWindowDestroy(window_);
            window_ = nullptr;
        }
        // Disconnect before either side is terminated, in the reverse of the order they were
        // joined. A connection left standing points at an object that is about to go away.
        if (component_point_ != nullptr && controller_point_ != nullptr) {
            auto* const a = *reinterpret_cast<v3_connection_point_cpp**>(component_point_);
            auto* const b = *reinterpret_cast<v3_connection_point_cpp**>(controller_point_);
            b->point.disconnect(controller_point_, component_point_);
            a->point.disconnect(component_point_, controller_point_);
        }
        Release(controller_point_);
        Release(component_point_);
        if (controller_ != nullptr) {
            auto* const vt = *reinterpret_cast<v3_edit_controller_cpp**>(controller_);
            vt->base.terminate(controller_);
            Release(controller_);
        }
        if (processor_ != nullptr && processing_) {
            auto* const vt = *reinterpret_cast<v3_audio_processor_cpp**>(processor_);
            vt->proc.set_processing(processor_, false);
        }
        Release(processor_);
        if (component_ != nullptr) {
            auto* const vt = *reinterpret_cast<v3_component_cpp**>(component_);
            if (active_) {
                vt->comp.set_active(component_, false);
            }
            vt->base.terminate(component_);
            Release(component_);
        }
        Release(factory_);
        // The module is left loaded. ModuleExit tears down every instance the library made, and
        // this host has no way to know whether another effect still holds one.
    }

    bool Load(const char* path, const char* class_name, double sample_rate, uint32_t max_frames)
    {
        wanted_class_ = class_name != nullptr ? class_name : "";
        const std::string binary = BinaryInsideBundle(path);
        if (binary.empty()) {
            Log("vst3: %s has no Contents/x86_64-linux binary", path);
            return false;
        }

        void* const handle = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            Log("vst3: dlopen(%s) failed: %s", binary.c_str(), dlerror());
            return false;
        }

        // Linux bundles must be entered before the factory is asked for anything.
        using ModuleEntryFn = bool (*)(void*);
        if (auto* const entry = reinterpret_cast<ModuleEntryFn>(dlsym(handle, "ModuleEntry"))) {
            if (!entry(handle)) {
                Log("vst3: ModuleEntry refused for %s", binary.c_str());
                return false;
            }
        }

        using GetFactoryFn = v3_plugin_factory** (*)(void);
        auto* const get_factory =
            reinterpret_cast<GetFactoryFn>(dlsym(handle, "GetPluginFactory"));
        if (get_factory == nullptr) {
            Log("vst3: %s exports no GetPluginFactory", binary.c_str());
            return false;
        }
        factory_ = get_factory();
        if (factory_ == nullptr) {
            Log("vst3: GetPluginFactory returned null for %s", binary.c_str());
            return false;
        }

        if (!CreateComponent(path)) {
            return false;
        }
        if (!SetUpAudio(sample_rate, max_frames)) {
            return false;
        }
        CreateController();  // an editor is optional; audio is not
        return true;
    }

    bool Process(float** buffers, uint32_t channel_count, uint32_t frames) override
    {
        if (processor_ == nullptr || buffers == nullptr || frames == 0 || frames > max_frames_) {
            return false;
        }
        if (channel_count != channel_count_) {
            return false;
        }

        // VST3 wants separate input and output pointers, and Resolve hands us one buffer that is
        // both. The input is copied into scratch and the plugin writes into Resolve's buffer.
        for (uint32_t channel = 0; channel < channel_count; ++channel) {
            std::memcpy(scratch_pointers_[channel], buffers[channel], frames * sizeof(float));
        }

        v3_audio_bus_buffers input{};
        input.num_channels = static_cast<int32_t>(channel_count);
        input.channel_silence_bitset = 0;
        input.channel_buffers_32 = scratch_pointers_.data();

        v3_audio_bus_buffers output{};
        output.num_channels = static_cast<int32_t>(channel_count);
        output.channel_silence_bitset = 0;
        output.channel_buffers_32 = buffers;

        v3_process_data data{};
        data.process_mode = V3_REALTIME;
        data.symbolic_sample_size = V3_SAMPLE_32;
        data.nframes = static_cast<int32_t>(frames);
        data.num_input_buses = 1;
        data.num_output_buses = 1;
        data.inputs = &input;
        data.outputs = &output;

        // Everything the editor changed since the last block. try_lock rather than lock: the
        // editor thread holds this for a few instructions, and an audio thread that waits on a
        // GUI thread is a dropout. A missed block costs one buffer of latency on a knob move.
        process_calls_.fetch_add(1, std::memory_order_relaxed);

        int32_t queued = 0;
        if (param_lock_.try_lock()) {
            for (const ParamPoint& point : pending_) {
                queues_[queued].vtable = &g_queue_vtable;
                queues_[queued].id = point.id;
                queues_[queued].value = point.value;
                if (++queued >= kMaxQueuedParams) {
                    break;
                }
            }
            pending_.clear();
            param_lock_.unlock();
        }
        if (queued > 0) {
            // Whether the audio thread ever finds anything to deliver. Without this line, an
            // editor that sends nothing and a queue that is never drained look identical from
            // outside, and both end with a state that does not change.
            if (drains_logged_ < 12) {
                ++drains_logged_;
                Log("vst3: \"%s\" [%d] delivered %d parameter changes to the processor", name_,
                    serial_, queued);
            }
            changes_.vtable = &g_changes_vtable;
            changes_.queues = queues_;
            changes_.count = queued;
            data.input_params = reinterpret_cast<v3_param_changes**>(&changes_);
        }

        auto* const vt = *reinterpret_cast<v3_audio_processor_cpp**>(processor_);
        // A bridged VST3 dies the same way a bridged VST2 does: yabridge throws when its Wine host
        // is gone, and an exception leaving an audio callback aborts Resolve. One effect going
        // quiet is the correct outcome.
        try {
            return vt->proc.process(processor_, &data) == V3_OK;
        } catch (...) {
            if (!threw_) {
                threw_ = true;
                Log("vst3: \"%s\" threw while processing - block dropped, still in the path",
                    name_);
            }
            return false;
        }
    }

    uint32_t ChannelCount() const override { return channel_count_; }
    const char* Name() const override { return name_[0] != '\0' ? name_ : nullptr; }
    PluginFormat Format() const override { return PluginFormat::Vst3; }
    unsigned long EditorWindow() const override { return PluginWindowHandle(window_); }

    // The component's state - the processor half, which is the half that carries the settings.
    //
    // The controller keeps its own state as well, but only for things the processor does not need,
    // and a controller that is fed the component state through set_component_state ends up in
    // step. That is the order used on restore below, and it is the order the SDK's own host uses.
    // Both halves of a VST3, because a VST3 keeps its settings in two places.
    //
    // The component is the processor and the controller is the editor, and the SDK does not
    // require them to share anything: the host moves values between them. Saving only the
    // component was enough for plugins that keep everything processor-side, and useless for the
    // ones that do not. Measured on 2026-08-29: two Waves F6-RTA, bands moved on both, and both
    // component states came back byte-identical. The F6 publishes 215 parameters, so the count
    // was never the limit - what the editor does not report, the processor never learns.
    //
    // Layout after the twelve-byte header: a four-byte length, the component state, a four-byte
    // length, the controller state. The old component-only blobs carry the V3CS tag and are still
    // read, so nothing saved before this is lost.
    bool ReadStream(void* target, bool controller, std::vector<uint8_t>& into)
    {
        MemoryStream stream{&g_stream_vtable, &into, 0};
        auto** const handle = reinterpret_cast<v3_bstream**>(&stream);
        if (controller) {
            auto* const vt = *reinterpret_cast<v3_edit_controller_cpp**>(target);
            return vt->ctrl.get_state != nullptr &&
                   vt->ctrl.get_state(target, handle) == V3_OK;
        }
        auto* const vt = *reinterpret_cast<v3_component_cpp**>(target);
        return vt->comp.get_state != nullptr && vt->comp.get_state(target, handle) == V3_OK;
    }

    static void AppendLength(std::vector<uint8_t>& out, size_t length)
    {
        const uint32_t value = static_cast<uint32_t>(length);
        const auto* const raw = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), raw, raw + sizeof(value));
    }

    bool SaveState(std::vector<uint8_t>& out) override
    {
        if (component_ == nullptr) {
            return false;
        }
        std::vector<uint8_t> component;
        if (!ReadStream(component_, false, component)) {
            return false;
        }
        std::vector<uint8_t> controller;
        if (controller_ != nullptr && !ReadStream(controller_, true, controller)) {
            controller.clear();  // a controller with nothing to say is not a failure
        }

        StateBegin(out, kStateTagVst3Both);
        AppendLength(out, component.size());
        out.insert(out.end(), component.begin(), component.end());
        AppendLength(out, controller.size());
        out.insert(out.end(), controller.begin(), controller.end());
        return true;
    }

    // component->setState, then controller->setComponentState, then controller->setState. That
    // order is the SDK's, and it matters: the last one must not be overwritten by the others.
    bool WriteStream(void* target, int which, std::vector<uint8_t>& bytes)
    {
        MemoryStream stream{&g_stream_vtable, &bytes, 0};
        auto** const handle = reinterpret_cast<v3_bstream**>(&stream);
        if (which == 0) {
            auto* const vt = *reinterpret_cast<v3_component_cpp**>(target);
            return vt->comp.set_state != nullptr && vt->comp.set_state(target, handle) == V3_OK;
        }
        auto* const vt = *reinterpret_cast<v3_edit_controller_cpp**>(target);
        auto* const call = which == 1 ? vt->ctrl.set_component_state : vt->ctrl.set_state;
        return call != nullptr && call(target, handle) == V3_OK;
    }

    bool LoadState(const uint8_t* data, size_t size) override
    {
        if (component_ == nullptr) {
            return false;
        }
        size_t body = 0;

        // A blob from before the controller half was saved.
        if (const uint8_t* const old = StateBody(data, size, kStateTagVst3, &body)) {
            std::vector<uint8_t> component(old, old + body);
            if (!WriteStream(component_, 0, component)) {
                return false;
            }
            if (controller_ != nullptr) {
                std::vector<uint8_t> again(old, old + body);
                WriteStream(controller_, 1, again);
            }
            edits_logged_ = 0;
            return true;
        }

        const uint8_t* const payload = StateBody(data, size, kStateTagVst3Both, &body);
        if (payload == nullptr || body < sizeof(uint32_t)) {
            return false;
        }
        uint32_t length = 0;
        std::memcpy(&length, payload, sizeof(length));
        if (sizeof(uint32_t) + length + sizeof(uint32_t) > body) {
            return false;
        }
        std::vector<uint8_t> component(payload + sizeof(uint32_t),
                                       payload + sizeof(uint32_t) + length);
        const uint8_t* const after = payload + sizeof(uint32_t) + length;
        uint32_t controller_length = 0;
        std::memcpy(&controller_length, after, sizeof(controller_length));
        if (sizeof(uint32_t) + length + sizeof(uint32_t) + controller_length > body) {
            return false;
        }
        std::vector<uint8_t> controller(after + sizeof(uint32_t),
                                        after + sizeof(uint32_t) + controller_length);

        if (!WriteStream(component_, 0, component)) {
            return false;
        }
        if (controller_ != nullptr) {
            std::vector<uint8_t> again = component;
            WriteStream(controller_, 1, again);
            if (!controller.empty()) {
                WriteStream(controller_, 2, controller);
            }
        }
        // Everything logged so far was the controller's opening broadcast. What matters is what
        // arrives after this, which is the user.
        edits_logged_ = 0;
        return true;
    }

    bool OpenEditor() override
    {
        if (controller_ == nullptr) {
            Log("vst3: \"%s\" has no edit controller, so no editor", name_);
            return false;
        }
        if (view_ != nullptr) {
            return PluginWindowShow(window_);
        }

        auto* const controller_vt = *reinterpret_cast<v3_edit_controller_cpp**>(controller_);
        view_ = controller_vt->ctrl.create_view(controller_, "editor");
        if (view_ == nullptr) {
            Log("vst3: create_view returned nothing for \"%s\"", name_);
            return false;
        }

        auto* const vt = *reinterpret_cast<v3_plugin_view_cpp**>(view_);
        if (vt->view.is_platform_type_supported(view_, V3_VIEW_PLATFORM_TYPE_X11) != V3_OK) {
            Log("vst3: \"%s\" does not support an X11 embedded editor", name_);
            Release(view_);
            return false;
        }

        // Ask the view its size before making the window, so it is right the first time.
        unsigned int width = 800;
        unsigned int height = 600;
        v3_view_rect rect{};
        if (vt->view.get_size(view_, &rect) == V3_OK && rect.right > rect.left &&
            rect.bottom > rect.top) {
            width = static_cast<unsigned int>(rect.right - rect.left);
            height = static_cast<unsigned int>(rect.bottom - rect.top);
        }

        window_ = PluginWindowCreate(width, height, name_);
        const unsigned long handle = PluginWindowHandle(window_);
        if (handle == 0) {
            Release(view_);
            return false;
        }

        // set_frame comes before attached(), never after. On Linux the view uses the frame to
        // find the host's run loop, and it looks for it inside attached() itself - so a frame
        // handed over afterwards is handed over too late.
        run_loop_.window = window_;
        if (vt->view.set_frame(view_, run_loop_.Frame()) != V3_OK) {
            Log("vst3: set_frame refused for \"%s\"", name_);
        }

        const v3_result attached = vt->view.attached(view_, reinterpret_cast<void*>(handle),
                                                     V3_VIEW_PLATFORM_TYPE_X11);
        if (attached != V3_OK) {
            Log("vst3: attached() refused for \"%s\" with 0x%08x", name_,
                static_cast<unsigned>(attached));
            // Take the window down too. Leaving it up is the black panel this showed before.
            Release(view_);
            run_loop_.window = nullptr;
            PluginWindowDestroy(window_);
            window_ = nullptr;
            return false;
        }
        PluginWindowFlush(window_);

        // The editor is live now, so the run loop has to actually run. Its X11 socket and its
        // timers were registered from inside attached(), and nothing services them until this.
        HostMainRegister(this, 16);

        Log("vst3: editor open for \"%s\" at %ux%u", name_, width, height);
        return true;
    }

    void CloseEditor() override
    {
        std::lock_guard<std::mutex> held(HostMainLock());
        PluginWindowHide(window_);
    }

    // From the host main thread, with HostMainLock() already held.
    void OnHostMainTick() override
    {
        run_loop_.Service();

        // A parameter change reaches the processor inside a process call, and Resolve only makes
        // those while audio is running. Move a control with the transport stopped and the change
        // sits in the queue: the sound does not follow it, and neither does the saved state.
        //
        // The cure is the SDK's own: a process call with no audio in it, carrying only the
        // changes. The host issues it, not the plugin.
        //
        // No lock is taken against the audio thread, because the block counter already answers
        // the only question that matters. If a block ran since the last tick, audio is live and
        // it drained the queue itself. Two quiet ticks - about 32 ms - mean nothing is calling
        // process, so nothing can be racing this.
        const uint64_t blocks = process_calls_.load(std::memory_order_relaxed);
        const bool quiet = blocks == last_blocks_;
        last_blocks_ = blocks;
        if (!quiet) {
            quiet_ticks_ = 0;
            return;
        }
        if (++quiet_ticks_ < 2) {
            return;
        }
        FlushParameters();
    }

    void FlushParameters()
    {
        if (processor_ == nullptr) {
            return;
        }
        int32_t queued = 0;
        {
            const std::lock_guard<std::mutex> held(param_lock_);
            for (const ParamPoint& point : pending_) {
                queues_[queued].vtable = &g_queue_vtable;
                queues_[queued].id = point.id;
                queues_[queued].value = point.value;
                if (++queued >= kMaxQueuedParams) {
                    break;
                }
            }
            pending_.clear();
        }
        if (queued == 0) {
            return;
        }

        changes_.vtable = &g_changes_vtable;
        changes_.queues = queues_;
        changes_.count = queued;

        v3_process_data data{};
        data.process_mode = V3_REALTIME;
        data.symbolic_sample_size = V3_SAMPLE_32;
        data.nframes = 0;  // the whole point: parameters only, no audio
        data.input_params = reinterpret_cast<v3_param_changes**>(&changes_);

        auto* const vt = *reinterpret_cast<v3_audio_processor_cpp**>(processor_);
        try {
            vt->proc.process(processor_, &data);
        } catch (...) {
            if (!threw_) {
                threw_ = true;
                Log("vst3: \"%s\" threw while taking parameters with the transport stopped",
                    name_);
            }
            return;
        }
        if (flushes_logged_ < 8) {
            ++flushes_logged_;
            Log("vst3: \"%s\" [%d] took %d parameter changes with no audio running", name_,
                serial_, queued);
        }
    }

private:
    // Which Audio Module class in the file is the plugin.
    //
    // With no name asked for, the first one - which is right for an ordinary plugin, and is what
    // this did before shells were supported. With a name, the class that carries it: a shell is one
    // file that publishes many plugins, and the Waves WaveShell publishes 718 of them, so "the
    // first class" would have meant one menu entry called "Immersive Wrapper Mono" standing in for
    // the entire Waves catalogue.
    bool CreateComponent(const char* path)
    {
        auto* const factory_vt = *reinterpret_cast<v3_plugin_factory_cpp**>(factory_);
        const int32_t count = factory_vt->v1.num_classes(factory_);

        bool have_factory_2 = false;
        {
            void* probe = nullptr;
            if (Query(factory_, v3_plugin_factory_2_iid, &probe)) {
                have_factory_2 = true;
                Unknown(factory_)->unref(factory_);  // Query took a reference we do not keep
            }
        }

        // IPluginFactory3::set_host_context is deliberately NOT called here, and this is measured,
        // not cautious. The spec makes seating the host context the host's job, so the call was
        // added on 2026-08-25 — the factory accepted it (`set_host_context returned 0x00000000`)
        // and then the very next call never returned. Resolve hung at "load project 100%" while
        // restoring a saved ERA6 effect, and the project could not be opened at all. Without the
        // call the same plugin merely refuses and its audio passes through, which is survivable.
        //
        // So the deadlock lives after the context is seated, not in the seating. yabridge's VST3
        // wrapper calls back into the host context from the Wine side, and something on our side
        // of that callback does not answer. Do not re-enable this until the callback path is
        // understood; a hang that blocks project load is worse than a plugin that will not start.
        // Stacks were unavailable at the time: yama ptrace_scope blocks eu-stack and gdb without
        // root, so the next attempt needs `sudo sysctl kernel.yama.ptrace_scope=0` set first.
        (void)v3_plugin_factory_3_iid;

        bool saw_audio_module = false;
        for (int32_t index = 0; index < count; ++index) {
            v3_class_info info{};
            if (factory_vt->v1.get_class_info(factory_, index, &info) != V3_OK) {
                continue;
            }
            if (std::strcmp(info.category, "Audio Module Class") != 0) {
                continue;
            }
            saw_audio_module = true;
            if (!wanted_class_.empty() && wanted_class_ != info.name) {
                continue;
            }

            void* created = nullptr;
            if (factory_vt->v1.create_instance(factory_, info.class_id, v3_component_iid,
                                               &created) != V3_OK ||
                created == nullptr) {
                Log("vst3: create_instance failed for \"%s\"", info.name);
                continue;
            }
            component_ = static_cast<v3_component**>(created);

            auto* const vt = *reinterpret_cast<v3_component_cpp**>(component_);
            const v3_result started = vt->base.initialize(component_, g_host_context);
            if (started != V3_OK) {
                // Print the code. "Refused" alone sent one diagnosis down the wrong path already.
                Log("vst3: initialize refused for \"%s\" with 0x%08x", info.name,
                    static_cast<unsigned>(started));
                Release(component_);
                continue;
            }

            std::snprintf(name_, sizeof(name_), "%s", info.name);
            std::memcpy(class_id_, info.class_id, sizeof(v3_tuid));
            if (have_factory_2) {
                v3_class_info_2 info2{};
                auto* const v2 = &factory_vt->v2;
                if (v2->get_class_info_2(factory_, index, &info2) == V3_OK) {
                    Log("vst3: loaded \"%s\" by %s", info2.name, info2.vendor);
                }
            }
            return true;
        }

        // Say which of the two happened. The old message claimed the bundle had no audio class even
        // when a class was found and then rejected, which reads as a scanner fault instead of a
        // host fault.
        if (!wanted_class_.empty()) {
            Log("vst3: %s publishes no Audio Module class called \"%s\"", path,
                wanted_class_.c_str());
        } else if (saw_audio_module) {
            Log("vst3: %s has an Audio Module class but no instance would start", path);
        } else {
            Log("vst3: %s exposes no Audio Module class", path);
        }
        return false;
    }

    bool SetUpAudio(double sample_rate, uint32_t max_frames)
    {
        void* queried = nullptr;
        if (!Query(component_, v3_audio_processor_iid, &queried)) {
            Log("vst3: \"%s\" is not an audio processor", name_);
            return false;
        }
        processor_ = static_cast<v3_audio_processor**>(queried);

        auto* const component_vt = *reinterpret_cast<v3_component_cpp**>(component_);
        auto* const processor_vt = *reinterpret_cast<v3_audio_processor_cpp**>(processor_);

        if (processor_vt->proc.can_process_sample_size(processor_, V3_SAMPLE_32) != V3_OK) {
            Log("vst3: \"%s\" cannot process 32-bit samples", name_);
            return false;
        }

        if (component_vt->comp.get_bus_count(component_, V3_AUDIO, V3_INPUT) < 1 ||
            component_vt->comp.get_bus_count(component_, V3_AUDIO, V3_OUTPUT) < 1) {
            Log("vst3: \"%s\" has no main audio bus in one direction - not an effect", name_);
            return false;
        }

        v3_bus_info input_bus{};
        v3_bus_info output_bus{};
        component_vt->comp.get_bus_info(component_, V3_AUDIO, V3_INPUT, 0, &input_bus);
        component_vt->comp.get_bus_info(component_, V3_AUDIO, V3_OUTPUT, 0, &output_bus);

        // The bridge feeds one bus in and reads one bus out, so the narrower side decides.
        channel_count_ = static_cast<uint32_t>(
            input_bus.channel_count < output_bus.channel_count ? input_bus.channel_count
                                                               : output_bus.channel_count);
        if (channel_count_ == 0 || channel_count_ > 8) {
            Log("vst3: unusable channel count %u for \"%s\"", channel_count_, name_);
            return false;
        }

        v3_speaker_arrangement arrangement =
            channel_count_ >= 2 ? kArrangementStereo : kArrangementMono;
        if (processor_vt->proc.set_bus_arrangements(processor_, &arrangement, 1, &arrangement,
                                                    1) != V3_OK) {
            // Reported, not fatal: a plugin may keep its own arrangement and still process.
            Log("vst3: set_bus_arrangements refused for \"%s\"", name_);
        }

        component_vt->comp.activate_bus(component_, V3_AUDIO, V3_INPUT, 0, true);
        component_vt->comp.activate_bus(component_, V3_AUDIO, V3_OUTPUT, 0, true);

        v3_process_setup setup{};
        setup.process_mode = V3_REALTIME;
        setup.symbolic_sample_size = V3_SAMPLE_32;
        setup.max_block_size = static_cast<int32_t>(max_frames);
        setup.sample_rate = sample_rate;
        if (processor_vt->proc.setup_processing(processor_, &setup) != V3_OK) {
            Log("vst3: setup_processing refused for \"%s\"", name_);
            return false;
        }

        if (component_vt->comp.set_active(component_, true) != V3_OK) {
            Log("vst3: set_active refused for \"%s\"", name_);
            return false;
        }
        active_ = true;

        if (processor_vt->proc.set_processing(processor_, true) != V3_OK) {
            // Some plugins answer false here and process anyway, so this is reported only.
            Log("vst3: set_processing answered false for \"%s\"", name_);
        }
        processing_ = true;

        max_frames_ = max_frames;
        scratch_.assign(static_cast<size_t>(channel_count_) * max_frames, 0.0f);
        scratch_pointers_.resize(channel_count_);
        for (uint32_t channel = 0; channel < channel_count_; ++channel) {
            scratch_pointers_[channel] =
                scratch_.data() + static_cast<size_t>(channel) * max_frames;
        }

        Log("vst3: \"%s\" ready, %u channels, %.0f Hz, up to %u frames", name_, channel_count_,
            sample_rate, max_frames);
        return true;
    }

    // The controller is usually a second class with its own id. Only when the plugin says it has
    // none is the component itself asked for the interface.
    void CreateController()
    {
        auto* const component_vt = *reinterpret_cast<v3_component_cpp**>(component_);
        v3_tuid controller_id{};
        void* created = nullptr;

        if (component_vt->comp.get_controller_class_id(component_, controller_id) == V3_OK) {
            auto* const factory_vt = *reinterpret_cast<v3_plugin_factory_cpp**>(factory_);
            if (factory_vt->v1.create_instance(factory_, controller_id, v3_edit_controller_iid,
                                               &created) != V3_OK) {
                created = nullptr;
            }
        }
        if (created == nullptr) {
            void* queried = nullptr;
            if (Query(component_, v3_edit_controller_iid, &queried)) {
                created = queried;
            }
        }
        if (created == nullptr) {
            Log("vst3: \"%s\" exposes no edit controller", name_);
            return;
        }

        controller_ = static_cast<v3_edit_controller**>(created);
        auto* const vt = *reinterpret_cast<v3_edit_controller_cpp**>(controller_);
        if (vt->base.initialize(controller_, g_host_context) != V3_OK) {
            // A controller that came from query_interface is already initialised as the component.
            Log("vst3: controller initialize answered false for \"%s\"", name_);
        }
        vt->ctrl.set_component_handler(
            controller_, reinterpret_cast<v3_component_handler**>(&handler_));

        // How many parameters the controller admits to. This is the number that decides whether
        // the editor can reach the processor at all: an F6 has six bands of seven controls, so a
        // count of four means the controls being dragged are not host parameters, and no amount
        // of routing on this side will carry them.
        if (vt->ctrl.get_parameter_count != nullptr) {
            Log("vst3: \"%s\" [%d] publishes %d parameters", name_, serial_,
                vt->ctrl.get_parameter_count(controller_));
        }
        ConnectComponentAndController();
    }

    // In most plugins the component and the controller are two separate objects, and joining them
    // is the host's job, not theirs. A controller that was never connected has never heard from its
    // component - and a plugin in that state is entitled to refuse to build an editor, which is
    // exactly what Accentize SpectralBalance2 did on 2026-08-25:
    //
    //     vst3: create_view returned nothing for "SpectralBalance2"
    //
    // A single-object plugin answers both queries with the same pointer. Connecting that to itself
    // is not a no-op, it is a loop, so it is skipped.
    void ConnectComponentAndController()
    {
        void* from_component = nullptr;
        void* from_controller = nullptr;
        if (!Query(component_, v3_connection_point_iid, &from_component) ||
            !Query(controller_, v3_connection_point_iid, &from_controller)) {
            return;  // one object, or a plugin that does not use messages at all
        }
        if (from_component == from_controller) {
            Unknown(static_cast<v3_funknown**>(from_component))->unref(from_component);
            Unknown(static_cast<v3_funknown**>(from_controller))->unref(from_controller);
            return;
        }

        component_point_ = static_cast<v3_connection_point**>(from_component);
        controller_point_ = static_cast<v3_connection_point**>(from_controller);

        auto* const component_vt = *reinterpret_cast<v3_connection_point_cpp**>(component_point_);
        auto* const controller_vt = *reinterpret_cast<v3_connection_point_cpp**>(controller_point_);
        const v3_result one = component_vt->point.connect(component_point_, controller_point_);
        const v3_result two = controller_vt->point.connect(controller_point_, component_point_);
        Log("vst3: connected the component and the controller (0x%08x, 0x%08x)",
            static_cast<unsigned>(one), static_cast<unsigned>(two));
    }

    v3_plugin_factory** factory_ = nullptr;
    v3_component** component_ = nullptr;
    v3_audio_processor** processor_ = nullptr;
    v3_edit_controller** controller_ = nullptr;
    v3_plugin_view** view_ = nullptr;
    v3_connection_point** component_point_ = nullptr;
    v3_connection_point** controller_point_ = nullptr;
    // Set from the editor's thread, drained by the audio thread. See PluginParameterEdited.
    struct ParamPoint {
        v3_param_id id;
        double value;
    };
    std::mutex param_lock_;
    std::vector<ParamPoint> pending_;
    ParamQueue queues_[kMaxQueuedParams] = {};
    ParamChanges changes_ = {};
    EditorHandler handler_ = {&g_handler_vtable, this};
    int serial_ = ++g_plugin_serial;
    int edits_logged_ = 0;
    int drains_logged_ = 0;
    int flushes_logged_ = 0;
    std::atomic<uint64_t> process_calls_{0};
    uint64_t last_blocks_ = 0;
    int quiet_ticks_ = 0;

public:
    // One control moved in the plugin's own editor.
    //
    // The newest value per parameter is what the processor needs, so a second move of the same
    // knob before the next block replaces the first rather than queueing behind it.
    void ParameterEdited(v3_param_id id, double value)
    {
        // Two effects hosting the same plugin are two objects with one name, and the log could
        // not tell them apart. The serial can. Capped, because a knob drag is hundreds of these.
        if (edits_logged_ < 8) {
            ++edits_logged_;
            Log("vst3: editor of \"%s\" [%d] moved parameter %u to %.4f", name_, serial_,
                static_cast<unsigned>(id), value);
        }
        BridgeParameterChangedByEditor(this, static_cast<unsigned>(id));

        const std::lock_guard<std::mutex> held(param_lock_);
        for (ParamPoint& point : pending_) {
            if (point.id == id) {
                point.value = value;
                return;
            }
        }
        if (pending_.size() < static_cast<size_t>(kMaxQueuedParams)) {
            pending_.push_back({id, value});
        }
    }

private:
    PluginWindow* window_ = nullptr;
    EditorRunLoop run_loop_;

    // Only so the first throw is logged and the rest are not. It never disables the plugin.
    bool threw_ = false;
    bool active_ = false;
    bool processing_ = false;
    uint32_t channel_count_ = 0;
    uint32_t max_frames_ = 0;
    char name_[128] = {0};
    std::string wanted_class_;
    v3_tuid class_id_{};

    std::vector<float> scratch_;
    std::vector<float*> scratch_pointers_;
};

void PluginParameterEdited(Vst3Plugin* plugin, v3_param_id id, double value)
{
    if (plugin != nullptr) {
        plugin->ParameterEdited(id, value);
    }
}

}  // namespace

HostedPlugin* CreateVst3Plugin(const char* path, const char* class_name, double sample_rate,
                               uint32_t max_frames)
{
    auto* const plugin = new Vst3Plugin();
    if (plugin->Load(path, class_name, sample_rate, max_frames)) {
        return plugin;
    }
    delete plugin;
    return nullptr;
}

bool Vst3ListClasses(const char* path, std::vector<std::string>& out,
                     std::vector<std::string>* sub_categories)
{
    out.clear();
    if (sub_categories != nullptr) {
        sub_categories->clear();
    }
    if (path == nullptr) {
        return false;
    }

    const std::string binary = BinaryInsideBundle(path);
    if (binary.empty()) {
        Log("vst3: %s has no Contents/x86_64-linux binary", path);
        return false;
    }

    // The module is opened and deliberately never closed.
    //
    // dlclose on a yabridge module tears down the Wine host behind it, and the plugin the user then
    // picks from the menu would have to start a second one. Leaving it mapped costs a file handle
    // and makes the later load cheap. The scan runs once per Resolve start, so this cannot grow.
    void* const handle = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        Log("vst3: dlopen(%s) failed: %s", binary.c_str(), dlerror());
        return false;
    }

    using ModuleEntryFn = bool (*)(void*);
    if (auto* const enter = reinterpret_cast<ModuleEntryFn>(dlsym(handle, "ModuleEntry"))) {
        if (!enter(handle)) {
            // Not fatal for the caller and not a fault in this file: yabridge refuses here when the
            // bundle holds no Windows module, which is what a native Linux VST3 in the yabridge
            // directory looks like.
            Log("vst3: ModuleEntry refused for %s", binary.c_str());
            return false;
        }
    }

    using GetFactoryFn = v3_plugin_factory** (*)(void);
    auto* const get_factory = reinterpret_cast<GetFactoryFn>(dlsym(handle, "GetPluginFactory"));
    if (get_factory == nullptr) {
        Log("vst3: %s exports no GetPluginFactory", binary.c_str());
        return false;
    }
    v3_plugin_factory** const factory = get_factory();
    if (factory == nullptr) {
        Log("vst3: GetPluginFactory returned null for %s", binary.c_str());
        return false;
    }

    auto* const factory_vt = *reinterpret_cast<v3_plugin_factory_cpp**>(factory);

    // IPluginFactory2 carries the subcategory. It is asked for, never assumed: a factory only has
    // to implement version 1, and reading v2's slot off a vtable that ends after v1 is a call into
    // whatever follows it in memory.
    void** factory2 = nullptr;
    if (sub_categories != nullptr) {
        void* found = nullptr;
        if (factory_vt->query_interface(factory, v3_plugin_factory_2_iid, &found) == V3_OK &&
            found != nullptr) {
            factory2 = static_cast<void**>(found);
        }
    }

    const int32_t count = factory_vt->v1.num_classes(factory);
    for (int32_t index = 0; index < count; ++index) {
        v3_class_info info{};
        if (factory_vt->v1.get_class_info(factory, index, &info) != V3_OK) {
            continue;
        }
        if (std::strcmp(info.category, "Audio Module Class") != 0) {
            continue;
        }
        // The name is a fixed-width field and is not required to be terminated.
        char name[sizeof(info.name) + 1];
        std::memcpy(name, info.name, sizeof(info.name));
        name[sizeof(info.name)] = '\0';
        if (name[0] == '\0') {
            continue;
        }
        out.push_back(name);

        // Kept in step with out, so index N of one describes index N of the other. A class that
        // publishes nothing gets an empty string rather than being skipped, because a gap here
        // would silently file every later plugin under its neighbour's category.
        if (sub_categories == nullptr) {
            continue;
        }
        std::string declared;
        if (factory2 != nullptr) {
            auto* const vt = *reinterpret_cast<v3_plugin_factory_cpp**>(factory2);
            v3_class_info_2 info2{};
            if (vt->v2.get_class_info_2(factory2, index, &info2) == V3_OK) {
                char sub[sizeof(info2.sub_categories) + 1];
                std::memcpy(sub, info2.sub_categories, sizeof(info2.sub_categories));
                sub[sizeof(info2.sub_categories)] = '\0';
                declared = sub;
            }
        }
        sub_categories->push_back(declared);
    }
    if (factory2 != nullptr) {
        auto* const vt = *reinterpret_cast<v3_plugin_factory_cpp**>(factory2);
        vt->unref(factory2);
    }
    return !out.empty();
}

void Vst3PluginSetLogger(void (*logger)(const char*)) { g_logger = logger; }
