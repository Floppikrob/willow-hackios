/*
 * The libdrm of xf86drm.h (see there): Linux's DRM requests to the kernel's GPU driver as calls
 * of its IOKit user client. A request's number (the low byte of the ioctl code) is the call's
 * selector, its structure goes in and comes back as it is; a buffer's memory is the client
 * memory of the type that is the buffer's handle.
 *
 * freedreno knows its device by a file descriptor. It gets one that means nothing (/dev/null):
 * there is one GPU and one connection to it in a process.
 */
#include <errno.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#include "xf86drm.h"
#include "drm-uapi/msm_drm.h"

#define REDWOOD_GPU_SERVICE     "RedwoodGPU"
#define REDWOOD_GPU_SURFACE     0x7e            /* the request that is not Linux's */
#define REDWOOD_GPU_DESCRIBE    0xffffffffu     /* ... asked only what the buffer would be like */

static io_connect_t connection;
static int connection_fd = -1;

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	io_service_t service;

	(void)busid;
	(void)type;
	if (strcmp(name, "msm") != 0) {
		return -1;
	}
	if (connection_fd >= 0) {
		return dup(connection_fd);
	}
	service = IOServiceGetMatchingService(0, IOServiceMatching(REDWOOD_GPU_SERVICE));
	if (service == 0) {
		return -1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
		IOObjectRelease(service);
		return -1;
	}
	IOObjectRelease(service);
	connection_fd = open("/dev/null", O_RDWR);
	return dup(connection_fd);
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);

	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	int error = call((unsigned)(request & 0xff), arg, IOCPARM_LEN(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

static int submit_with_syncobjs(struct drm_msm_gem_submit *request, unsigned long size);

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	if (index == DRM_MSM_GEM_SUBMIT && size >= sizeof(struct drm_msm_gem_submit)) {
		return submit_with_syncobjs(data, size);
	}
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/*
 * The driver calls itself what Linux's is called, and is of the version whose interface it has:
 * 1.7, buffers at addresses the kernel chooses and tells ("softpin"), nothing newer.
 */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version = calloc(1, sizeof(*version));

	(void)fd;
	if (version != NULL) {
		version->version_major = 1;
		version->version_minor = 7;
		version->name = strdup("msm");
		version->name_len = 3;
		version->date = strdup("");
		version->desc = strdup("Adreno on Darwin");
		version->desc_len = (int)strlen(version->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

void *
redwood_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)fd;
	(void)size;
	/*
	 * Linux gives freedreno its buffers write-combined, with no cache of this processor's between
	 * the program and the memory the GPU sees. So does the kernel's driver here: the memory is
	 * made that way, and this mapping is as the memory is.
	 */
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	return (void *)(uintptr_t)address;
}

void
redwood_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
redwood_drm_surface(int fd, struct redwood_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = REDWOOD_GPU_DESCRIBE;
	error = call(REDWOOD_GPU_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct redwood_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(REDWOOD_GPU_SURFACE, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* Buffers and fences as file descriptors, for handing them to other programs and drivers: none of that here. */
#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
NONE(drmSyncobjQuery, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles,
    __attribute__((unused)) uint64_t *points, __attribute__((unused)) uint32_t count)
NONE(drmSyncobjTimelineSignal, __attribute__((unused)) int fd, __attribute__((unused)) const uint32_t *handles,
    __attribute__((unused)) uint64_t *points, __attribute__((unused)) uint32_t count)
NONE(drmSyncobjTimelineWait, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles,
    __attribute__((unused)) uint64_t *points, __attribute__((unused)) unsigned count,
    __attribute__((unused)) int64_t timeout_nsec, __attribute__((unused)) unsigned flags,
    __attribute__((unused)) uint32_t *first_signaled)
NONE(drmSyncobjTransfer, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t dst_handle,
    __attribute__((unused)) uint64_t dst_point, __attribute__((unused)) uint32_t src_handle,
    __attribute__((unused)) uint64_t src_point, __attribute__((unused)) uint32_t flags)
NONE(drmGetDevice2, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t flags,
    __attribute__((unused)) drmDevicePtr *device)

void
drmFreeDevice(drmDevicePtr *device)
{
	(void)device;
}

int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a == b;
}

/*
 * ---- synchronisation objects, in the process ----
 *
 * Linux keeps them in the kernel: a fence each, that a submission is given to signal and another
 * waits for. Here the kernel knows only its submissions' numbers, which a wait can be asked
 * about (MSM_WAIT_FENCE), so an object is kept here: signalled, or waiting for submission N, or
 * neither. A submission with objects to wait for (MSM_SUBMIT_SYNCOBJ_IN) waits for them here
 * before it goes to the kernel; those it is to signal (MSM_SUBMIT_SYNCOBJ_OUT) are given its
 * number when it comes back. One process's objects: there is no passing them to another one.
 */
#define SYNCOBJS 4096

static struct {
	bool used, signalled;
	uint32_t fence, queue;                  /* waiting for that submission (0: for nothing yet) */
} syncobjs[SYNCOBJS];
static pthread_mutex_t syncobjs_lock = PTHREAD_MUTEX_INITIALIZER;

static int
fail(int error)
{
	errno = error;
	return -error;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	pthread_mutex_lock(&syncobjs_lock);
	for (uint32_t i = 0; i < SYNCOBJS; i++) {
		if (!syncobjs[i].used) {
			syncobjs[i].used = true;
			syncobjs[i].signalled = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0;
			syncobjs[i].fence = 0;
			pthread_mutex_unlock(&syncobjs_lock);
			*handle = i + 1;
			return 0;
		}
	}
	pthread_mutex_unlock(&syncobjs_lock);
	return fail(ENOMEM);
}

static bool
valid(uint32_t handle)
{
	return handle >= 1 && handle <= SYNCOBJS && syncobjs[handle - 1].used;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	(void)fd;
	pthread_mutex_lock(&syncobjs_lock);
	if (!valid(handle)) {
		pthread_mutex_unlock(&syncobjs_lock);
		return fail(EINVAL);
	}
	syncobjs[handle - 1].used = false;
	pthread_mutex_unlock(&syncobjs_lock);
	return 0;
}

static int
set_state(const uint32_t *handles, uint32_t count, bool signalled)
{
	pthread_mutex_lock(&syncobjs_lock);
	for (uint32_t i = 0; i < count; i++) {
		if (!valid(handles[i])) {
			pthread_mutex_unlock(&syncobjs_lock);
			return fail(EINVAL);
		}
		syncobjs[handles[i] - 1].signalled = signalled;
		syncobjs[handles[i] - 1].fence = 0;
	}
	pthread_mutex_unlock(&syncobjs_lock);
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	return set_state(handles, count, false);
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	return set_state(handles, count, true);
}

/* Now, on the clock the kernel's timeouts are on (iokit/Kernel/RedwoodGPU.cpp: millisecondsUntil). */
static uint64_t
continuous_ns(void)
{
	static mach_timebase_info_data_t timebase;

	if (timebase.denom == 0) {
		mach_timebase_info(&timebase);
	}
	return mach_continuous_time() * timebase.numer / timebase.denom;
}

/* Whether submission `fence` is done; waits up to a millisecond for it. */
static bool
fence_done(uint32_t fence, uint32_t queue)
{
	uint64_t until = continuous_ns() + 1000000;
	struct drm_msm_wait_fence request = {
		.fence = fence,
		.timeout = { .tv_sec = (int64_t)(until / 1000000000), .tv_nsec = (int64_t)(until % 1000000000) },
		.queueid = queue,
	};

	return call(DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE, &request, sizeof(request)) == 0;
}

/* One object's state: 1 signalled, 0 not yet, -1 not even given a submission. */
static int
syncobj_state(uint32_t handle)
{
	uint32_t fence, queue;

	pthread_mutex_lock(&syncobjs_lock);
	if (syncobjs[handle - 1].signalled) {
		pthread_mutex_unlock(&syncobjs_lock);
		return 1;
	}
	fence = syncobjs[handle - 1].fence;
	queue = syncobjs[handle - 1].queue;
	pthread_mutex_unlock(&syncobjs_lock);
	if (fence == 0) {
		return -1;
	}
	if (!fence_done(fence, queue)) {
		return 0;
	}
	pthread_mutex_lock(&syncobjs_lock);
	if (syncobjs[handle - 1].fence == fence) {
		syncobjs[handle - 1].signalled = true;
	}
	pthread_mutex_unlock(&syncobjs_lock);
	return 1;
}

/* timeout_nsec: an absolute time on CLOCK_MONOTONIC, as libdrm's */
int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	(void)fd;
	for (unsigned i = 0; i < count; i++) {
		if (!valid(handles[i])) {
			return fail(EINVAL);
		}
	}
	for (;;) {
		unsigned done = 0, first = count;
		struct timespec now;

		for (unsigned i = 0; i < count; i++) {
			int state = syncobj_state(handles[i]);

			if (state < 0 && !(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)) {
				return fail(EINVAL);
			}
			if (state > 0) {
				done++;
				if (first == count) {
					first = i;
				}
			}
		}
		if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) ? done == count : done > 0) {
			if (first_signaled != NULL) {
				*first_signaled = first;
			}
			return 0;
		}
		clock_gettime(CLOCK_MONOTONIC, &now);
		if ((int64_t)now.tv_sec * 1000000000 + now.tv_nsec >= timeout_nsec) {
			return fail(ETIME);
		}
		usleep(50);
	}
}

static int
submit_with_syncobjs(struct drm_msm_gem_submit *request, unsigned long size)
{
	uint32_t flags = request->flags, stride = request->syncobj_stride ? request->syncobj_stride :
	    sizeof(struct drm_msm_gem_submit_syncobj);
	uint64_t ins = request->in_syncobjs, outs = request->out_syncobjs;
	uint32_t nr_in = request->nr_in_syncobjs, nr_out = request->nr_out_syncobjs;
	int error;

	if (flags & MSM_SUBMIT_SYNCOBJ_IN) {
		for (uint32_t i = 0; i < nr_in; i++) {
			struct drm_msm_gem_submit_syncobj *in = (void *)(uintptr_t)(ins + (uint64_t)i * stride);
			uint32_t handle = in->handle;
			struct timespec now;

			clock_gettime(CLOCK_MONOTONIC, &now);
			if (!valid(handle) || drmSyncobjWait(-1, &handle, 1, (int64_t)now.tv_sec * 1000000000 + now.tv_nsec +
			    5000000000ll, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL) != 0) {
				return fail(EINVAL);
			}
			if (in->flags & MSM_SUBMIT_SYNCOBJ_RESET) {
				drmSyncobjReset(-1, &handle, 1);
			}
		}
	}
	request->flags &= ~(MSM_SUBMIT_SYNCOBJ_IN | MSM_SUBMIT_SYNCOBJ_OUT);
	request->in_syncobjs = request->out_syncobjs = 0;
	request->nr_in_syncobjs = request->nr_out_syncobjs = 0;
	error = call(DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT, request, size);
	request->flags = flags;
	request->in_syncobjs = ins;
	request->out_syncobjs = outs;
	request->nr_in_syncobjs = nr_in;
	request->nr_out_syncobjs = nr_out;
	if (error != 0) {
		return -error;
	}
	if (flags & MSM_SUBMIT_SYNCOBJ_OUT) {
		pthread_mutex_lock(&syncobjs_lock);
		for (uint32_t i = 0; i < nr_out; i++) {
			struct drm_msm_gem_submit_syncobj *out = (void *)(uintptr_t)(outs + (uint64_t)i * stride);

			if (valid(out->handle)) {
				syncobjs[out->handle - 1].signalled = false;
				syncobjs[out->handle - 1].fence = request->fence;
				syncobjs[out->handle - 1].queue = request->queueid;
			}
		}
		pthread_mutex_unlock(&syncobjs_lock);
	}
	return 0;
}
NONE(drmSyncobjFDToHandle, __attribute__((unused)) int fd, __attribute__((unused)) int obj_fd,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *obj_fd)
NONE(drmSyncobjImportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int sync_file_fd)
NONE(drmSyncobjExportSyncFile, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *sync_file_fd)
