/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Included by ps5drv.c and the host contract fixture. */
/* Local state is serialized with screen_lock; never call user32 under it. */
#include "pw_d3d9_window_driver.h"
#include "pw_d3d9_window.c"

struct bridge_window_owner
{
    HWND hwnd;
    DWORD tid;
    uint32_t guest_generation, guest_epoch;
    uint64_t token;
    BOOL guest, destroyed;
};
static struct bridge_window_owner bridge_owners[256];
static uint32_t bridge_guest_generations[256];
static uint32_t bridge_guest_epoch;
static struct pw_d3d9_windows bridge_windows;
static uint64_t bridge_token;
static uint32_t bridge_epoch;

static uint64_t bridge_current_token(void)
{
    struct { UINT version, size; uint64_t token; } info = {1, 16, 0};
    if (NtCurrentTeb()->WowTebOffset ||
        NtQueryInformationThread( NtCurrentThread(), 0x50570002, &info, sizeof(info), NULL )) return 0;
    return info.token;
}

static struct bridge_window_owner *bridge_owner( HWND hwnd )
{
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(bridge_owners); ++i)
        if (bridge_owners[i].hwnd == hwnd) return &bridge_owners[i];
    return NULL;
}

static struct bridge_window_owner *bridge_guest_owner( struct pw_d3d9_window_id id )
{
    struct bridge_window_owner *owner;
    if (!id.epoch || !id.id || id.id > ARRAY_SIZE(bridge_owners) || !id.generation) return NULL;
    owner = &bridge_owners[id.id - 1];
    return owner->hwnd && owner->guest && owner->guest_generation == id.generation && owner->guest_epoch == id.epoch ? owner : NULL;
}

ULONG_PTR ps5_bridge_guest_window_call( HWND hwnd, void *ptr )
{
    struct pw_d3d9_guest_window_request q;
    struct pw_d3d9_window_id association;
    struct bridge_window_owner *owner;
    DWORD pid = 0, tid;
    unsigned index;
    int result = PW_D3D9_WINDOW_INVALID;

    if (!NtCurrentTeb()->WowTebOffset || !hwnd || !ptr) return result;
    memcpy( &q, ptr, sizeof(q) );
    if (q.version != PW_D3D9_GUEST_WINDOW_VERSION || q.size != sizeof(q) ||
        q.reserved || q.reserved2) return result;
    tid = get_window_thread( hwnd, &pid );
    if (!tid || tid != GetCurrentThreadId() || pid != GetCurrentProcessId()) return result;
    pthread_mutex_lock( &screen_lock );
    owner = bridge_owner( hwnd );
    if (!owner || !owner->guest || owner->destroyed || owner->token || owner->tid != tid) goto done;
    index = owner - bridge_owners;
    if (q.operation == PW_D3D9_GUEST_REGISTER)
    {
        if (q.id.epoch || q.id.id || q.id.generation) goto done;
        if (!owner->guest_generation)
        {
            if (bridge_guest_generations[index] == UINT32_MAX || bridge_guest_epoch == UINT32_MAX) { result = PW_D3D9_WINDOW_EXHAUSTED; goto done; }
            owner->guest_generation = ++bridge_guest_generations[index];
            owner->guest_epoch = ++bridge_guest_epoch;
        }
        q.id = (struct pw_d3d9_window_id){owner->guest_epoch, index+1, owner->guest_generation};
        result = PW_D3D9_WINDOW_OK;
    }
    else if (q.operation == PW_D3D9_GUEST_UNREGISTER)
    {
        if (bridge_guest_owner( q.id ) != owner) { result = PW_D3D9_WINDOW_STALE; goto done; }
        if (!pw_d3d9_window_find( &bridge_windows, (UINT_PTR)hwnd, &association ))
        { result = PW_D3D9_WINDOW_BUSY; goto done; }
        owner->guest_generation = owner->guest_epoch = 0;
        q.id = (struct pw_d3d9_window_id){0};
        result = PW_D3D9_WINDOW_OK;
    }
done:
    pthread_mutex_unlock( &screen_lock );
    if (!result) memcpy( ptr, &q, sizeof(q) );
    return result;
}

static BOOL bridge_owner_full(void)
{
    unsigned i;
    for (i = 0; i < ARRAY_SIZE(bridge_owners); ++i) if (!bridge_owners[i].hwnd) return FALSE;
    return TRUE;
}

static BOOL bridge_service_window( HWND hwnd )
{
    struct pw_d3d9_window_id id;
    struct pw_d3d9_window_entry e;
    struct bridge_window_owner *owner = bridge_owner( hwnd );
    if ((!owner && bridge_owner_full()) || (owner && owner->token)) return TRUE;
    return !pw_d3d9_window_find( &bridge_windows, (UINT_PTR)hwnd, &id ) &&
           !pw_d3d9_window_get( &bridge_windows, id, &e ) && e.service == (UINT_PTR)hwnd;
}

/* screen_lock held; NULL means known association suppresses input. */
static HWND bridge_input_window( HWND hwnd )
{
    struct pw_d3d9_window_id id;
    if (pw_d3d9_window_find( &bridge_windows, (UINT_PTR)hwnd, &id )) return bridge_service_window( hwnd ) ? NULL : hwnd;
    return (HWND)(UINT_PTR)pw_d3d9_window_input( &bridge_windows, (UINT_PTR)hwnd );
}

#ifdef SONAME_LIBVULKAN
struct bridge_surface
{
    BOOL reserved;
    VkInstance instance;
    VkSurfaceKHR surface;
    struct pw_d3d9_window_lease lease;
};
static struct bridge_surface bridge_surfaces[PW_D3D9_WINDOW_LEASES];
#endif

ULONG_PTR ps5_bridge_window_call( void *ptr, ULONG_PTR size )
{
    struct pw_d3d9_window_driver_request q;
    struct bridge_window_owner *guest, *service;
    struct pw_d3d9_window_entry e;
    uint64_t token = bridge_current_token();
    HWND guest_hwnd;
    DWORD guest_pid = 0, service_pid = 0, guest_tid, service_tid;
    unsigned i;
    int result = PW_D3D9_WINDOW_INVALID;

    if (!token || !ptr || size != sizeof(q)) return result;
    memcpy( &q, ptr, sizeof(q) );
    if (q.version != PW_D3D9_WINDOW_DRIVER_VERSION || q.size != sizeof(q) || q.reserved || q.reserved2)
        return result;
    /* Resolve opaque ID under our lock, then query Wine outside it. */
    pthread_mutex_lock( &screen_lock );
    guest = bridge_guest_owner( q.guest );
    guest_hwnd = guest ? guest->hwnd : NULL;
    pthread_mutex_unlock( &screen_lock );
    if (!guest_hwnd) return PW_D3D9_WINDOW_STALE;
    guest_tid = get_window_thread( guest_hwnd, &guest_pid );
    service_tid = get_window_thread( (HWND)(UINT_PTR)q.service, &service_pid );
    if ((q.operation == PW_D3D9_WINDOW_ATTACH || q.operation == PW_D3D9_WINDOW_BEGIN) &&
        (!guest_tid || !service_tid || guest_pid != GetCurrentProcessId() ||
         service_pid != GetCurrentProcessId() || service_tid != GetCurrentThreadId())) return result;

    pthread_mutex_lock( &screen_lock );
    guest = bridge_guest_owner( q.guest );
    service = bridge_owner( (HWND)(UINT_PTR)q.service );
    if (!guest || guest->hwnd != guest_hwnd || !service || !guest->guest || guest->token || service->guest ||
        service->token != token || service->tid != GetCurrentThreadId()) goto done;
    if ((q.operation == PW_D3D9_WINDOW_ATTACH || q.operation == PW_D3D9_WINDOW_BEGIN) &&
        (guest->destroyed || service->destroyed || guest->tid != guest_tid || service->tid != service_tid)) goto done;
    if (q.operation == PW_D3D9_WINDOW_ATTACH)
    {
        if (bridge_token && bridge_token != token) { result = PW_D3D9_WINDOW_BUSY; goto done; }
        if (!bridge_token)
        {
#ifdef SONAME_LIBVULKAN
            for (i = 0; i < ARRAY_SIZE(bridge_surfaces); ++i)
                if (bridge_surfaces[i].reserved) { result = PW_D3D9_WINDOW_BUSY; goto done; }
#endif
            if (bridge_epoch == UINT32_MAX) { result = PW_D3D9_WINDOW_EXHAUSTED; goto done; }
            pw_d3d9_windows_init( &bridge_windows, ++bridge_epoch );
            bridge_token = token;
        }
        result = pw_d3d9_window_attach( &bridge_windows, (UINT_PTR)guest_hwnd, q.service, &q.id );
    }
    else
    {
        if (bridge_token != token || pw_d3d9_window_get( &bridge_windows, q.id, &e ) ||
            e.guest != (UINT_PTR)guest_hwnd || e.service != q.service) { result = PW_D3D9_WINDOW_STALE; goto done; }
        switch (q.operation)
        {
        case PW_D3D9_WINDOW_BEGIN: result = pw_d3d9_window_begin( &bridge_windows, q.id, q.sequence, &q.state ); break;
        case PW_D3D9_WINDOW_ACK: result = pw_d3d9_window_ack( &bridge_windows, q.id, q.sequence, q.hresult ); break;
        case PW_D3D9_WINDOW_CLOSE: result = pw_d3d9_window_close( &bridge_windows, q.id ); break;
        case PW_D3D9_WINDOW_DETACH:
            result = pw_d3d9_window_detach( &bridge_windows, q.id );
            if (!result)
            {
                if (guest->destroyed) memset( guest, 0, sizeof(*guest) );
                if (service->destroyed) memset( service, 0, sizeof(*service) );
            }
            break;
        }
    }
    for (i = 0; i < PW_D3D9_WINDOWS; ++i) if (bridge_windows.windows[i].live) break;
    if (i == PW_D3D9_WINDOWS) bridge_token = 0;
done:
    pthread_mutex_unlock( &screen_lock );
    if (!result) memcpy( ptr, &q, sizeof(q) );
    return result;
}

/* Called by real driver window lifetime hooks, including hidden windows. */
static int bridge_window_created( HWND hwnd )
{
    uint64_t token = bridge_current_token();
    unsigned i;
    pthread_mutex_lock( &screen_lock );
    for (i = 0; i < ARRAY_SIZE(bridge_owners); ++i) if (!bridge_owners[i].hwnd)
    {
        bridge_owners[i] = (struct bridge_window_owner){hwnd, GetCurrentThreadId(), 0, 0, token,
                                                       NtCurrentTeb()->WowTebOffset != 0, FALSE};
        break;
    }
    pthread_mutex_unlock( &screen_lock );
    return token ? (i == ARRAY_SIZE(bridge_owners) ? -1 : 1) : 0;
}

/* screen_lock held. Keep tombstones until pending work and leases drain. */
static void bridge_window_destroyed( HWND hwnd )
{
    struct pw_d3d9_window_id id;
    struct bridge_window_owner *owner = bridge_owner( hwnd );
    if (!pw_d3d9_window_find( &bridge_windows, (UINT_PTR)hwnd, &id ))
    {
        pw_d3d9_window_close( &bridge_windows, id );
        if (owner) owner->destroyed = TRUE;
    }
    else if (owner) memset( owner, 0, sizeof(*owner) );
}

#ifdef SONAME_LIBVULKAN
/* Reservation precedes any display ownership side effects. */
static struct bridge_surface *bridge_surface_reserve( struct client_surface *client )
{
    struct bridge_surface *slot = NULL;
    struct pw_d3d9_window_id candidates[PW_D3D9_WINDOWS], selected = {0};
    uint64_t services[PW_D3D9_WINDOWS], selected_service = 0;
    uint64_t token = bridge_current_token(), captured_token;
    unsigned i, count = 0;

    pthread_mutex_lock( &screen_lock );
    captured_token = bridge_token;
    if (token && token == captured_token)
        for (i = 0; i < PW_D3D9_WINDOWS; ++i) if (bridge_windows.windows[i].live)
        {
            candidates[count] = (struct pw_d3d9_window_id){bridge_windows.epoch, i+1, bridge_windows.windows[i].generation};
            services[count++] = bridge_windows.windows[i].service;
        }
    pthread_mutex_unlock( &screen_lock );
    /* This takes win32u's surfaces_lock. Never nest it under screen_lock. */
    for (i = 0; i < count; ++i)
        if (is_client_surface_window( client, (HWND)(UINT_PTR)services[i] ))
        { selected = candidates[i]; selected_service = services[i]; break; }

    pthread_mutex_lock( &screen_lock );
    if (captured_token != bridge_token || (token && !selected.id) || (bridge_token && token != bridge_token)) goto done;
    for (i = 0; i < ARRAY_SIZE(bridge_surfaces); ++i)
        if (!bridge_surfaces[i].reserved) { slot = &bridge_surfaces[i]; break; }
    if (!slot) goto done;
    memset( slot, 0, sizeof(*slot) );
    if (bridge_token && pw_d3d9_window_acquire( &bridge_windows, selected, selected_service, &slot->lease ))
    { slot = NULL; goto done; }
    slot->reserved = TRUE;
done:
    pthread_mutex_unlock( &screen_lock );
    return slot;
}

static void bridge_surface_finish( struct bridge_surface *slot, VkInstance instance,
                                   VkSurfaceKHR surface, VkResult result )
{
    pthread_mutex_lock( &screen_lock );
    if (result == VK_SUCCESS) { slot->instance = instance; slot->surface = surface; }
    else
    {
        if (slot->lease.id) pw_d3d9_window_release( &bridge_windows, slot->lease );
        memset( slot, 0, sizeof(*slot) );
    }
    pthread_mutex_unlock( &screen_lock );
}

/* Called only after the actual host driver has destroyed this surface. */
void ps5_bridge_surface_destroyed( UINT_PTR instance, UINT64 surface )
{
    unsigned i;
    pthread_mutex_lock( &screen_lock );
    for (i = 0; i < ARRAY_SIZE(bridge_surfaces); ++i)
    {
        struct bridge_surface *slot = &bridge_surfaces[i];
        if (!slot->reserved || (UINT_PTR)slot->instance != instance || (UINT64)slot->surface != surface) continue;
        if (slot->lease.id) pw_d3d9_window_release( &bridge_windows, slot->lease );
        memset( slot, 0, sizeof(*slot) );
        break;
    }
    pthread_mutex_unlock( &screen_lock );
}

#endif
