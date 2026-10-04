/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Public Wine server function excerpts for the bounded native authority model.
 * Copyright (C) 1998 Alexandre Julliard (Wine mutex/handle implementation).
 * Source pins below are hashes of the full input units, not a Wine execution claim.
 */

/* server/mutex.c SHA256 6cd0e904781bed2f947cd6644dff31ce57b28620203c945c56c6d789f93fd6f9 */

static void do_grab( struct mutex_sync *mutex, struct thread *thread )
{
    assert( !mutex->count || (mutex->owner == thread) );

    if (!mutex->count++)  /* FIXME: avoid wrap-around */
    {
        assert( !mutex->owner );
        grab_object( mutex );
        mutex->owner = thread;
        list_add_head( &thread->mutex_list, &mutex->entry );
    }
}

static int do_release( struct mutex_sync *mutex, struct thread *thread, int count )
{
    if (!mutex->count || (mutex->owner != thread))
    {
        set_error( STATUS_MUTANT_NOT_OWNED );
        return 0;
    }
    if (!(mutex->count -= count))
    {
        /* remove the mutex from the thread list of owned mutexes */
        list_remove( &mutex->entry );
        mutex->owner = NULL;
        wake_up( &mutex->obj, 0 );
        release_object( mutex );
    }
    return 1;
}

void abandon_mutexes( struct thread *thread )
{
    struct list *ptr;

    while ((ptr = list_head( &thread->mutex_list )) != NULL)
    {
        struct mutex_sync *mutex = LIST_ENTRY( ptr, struct mutex_sync, entry );
        assert( mutex->owner == thread );
        mutex->abandoned = 1;
        do_release( mutex, thread, mutex->count );
    }

    abandon_inproc_mutexes( thread->id );
}

/* server/thread.c SHA256 fd0af5d4df74c666467cca2ae873addecccd93123cfa2f8787d86e90a831d192 */

static inline int is_thread_suspended( struct thread *thread )
{
    if (thread->context && thread->context->cooperative) return 0;
    if (thread->suspend) return 1;
    return !thread->bypass_proc_suspend && thread->process->suspend;
}

/* server/handle.c SHA256 9deadf724d7630f58c0122c85ffdd681bacaab059b0fcaffec69b1b27d4728b4 */

struct object *get_handle_obj( struct process *process, obj_handle_t handle,
                               unsigned int access, const struct object_ops *ops )
{
    struct handle_entry *entry;
    struct object *obj;

    if (!(obj = get_magic_handle( handle )))
    {
        if (!(entry = get_handle( process, handle )))
        {
            set_error( STATUS_INVALID_HANDLE );
            return NULL;
        }
        obj = entry->ptr;
        if (ops && (obj->ops != ops))
        {
            set_error( STATUS_OBJECT_TYPE_MISMATCH );  /* not the right type */
            return NULL;
        }
        if ((entry->access & access) != access)
        {
            set_error( STATUS_ACCESS_DENIED );
            return NULL;
        }
    }
    else if (ops && (obj->ops != ops))
    {
        set_error( STATUS_OBJECT_TYPE_MISMATCH );  /* not the right type */
        return NULL;
    }
    return grab_object( obj );
}
