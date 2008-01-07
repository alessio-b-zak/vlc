/*****************************************************************************
 * media_instance.c: Libvlc API Media Instance management functions
 *****************************************************************************
 * Copyright (C) 2005 the VideoLAN team
 * $Id$
 *
 * Authors: Clément Stenac <zorglub@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <vlc/libvlc.h>
#include <vlc_demux.h>
#include <vlc_input.h>
#include "libvlc_internal.h"
#include "libvlc.h"

static const libvlc_state_t vlc_to_libvlc_state_array[] =
{
    [INIT_S]        = libvlc_Opening,
    [OPENING_S]     = libvlc_Opening,
    [BUFFERING_S]   = libvlc_Buffering,    
    [PLAYING_S]     = libvlc_Playing,    
    [PAUSE_S]       = libvlc_Paused,    
    [END_S]         = libvlc_Ended,    
    [ERROR_S]       = libvlc_Error,    
};
static inline libvlc_state_t vlc_to_libvlc_state( int vlc_state )
{
    if( vlc_state < 0 || vlc_state > 6 )
        return libvlc_Stopped;

    return vlc_to_libvlc_state_array[vlc_state];
}

/*
 * Release the associated input thread
 *
 * Object lock is NOT held.
 */
static void release_input_thread( libvlc_media_instance_t *p_mi )
{
    input_thread_t *p_input_thread;

    if( !p_mi || p_mi->i_input_id == -1 )
        return;

    p_input_thread = (input_thread_t*)vlc_object_get(
                                             p_mi->p_libvlc_instance->p_libvlc_int,
                                             p_mi->i_input_id );

    p_mi->i_input_id = -1;

    if( !p_input_thread )
        return;
 
    /* release for previous vlc_object_get */
    vlc_object_release( p_input_thread );

    /* release for initial p_input_thread yield (see _new()) */
    vlc_object_release( p_input_thread );

    /* No one is tracking this input_thread appart us. Destroy it */
    if( p_mi->b_own_its_input_thread )
    {
        /* We owned this one */
        input_StopThread( p_input_thread );
        var_Destroy( p_input_thread, "drawable" );
        input_DestroyThread( p_input_thread );
    }
    else
    {
        /* XXX: hack the playlist doesn't retain the input thread,
         * so we did it for the playlist (see _new_from_input_thread),
         * revert that here. This will be deleted with the playlist API */
        vlc_object_release( p_input_thread );
    }
}

/*
 * Retrieve the input thread. Be sure to release the object
 * once you are done with it. (libvlc Internal)
 *
 * Object lock is held.
 */
input_thread_t *libvlc_get_input_thread( libvlc_media_instance_t *p_mi,
                                         libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;

    vlc_mutex_lock( &p_mi->object_lock );

    if( !p_mi || p_mi->i_input_id == -1 )
    {
        vlc_mutex_unlock( &p_mi->object_lock );
        RAISENULL( "Input is NULL" );
    }

    p_input_thread = (input_thread_t*)vlc_object_get(
                                             p_mi->p_libvlc_instance->p_libvlc_int,
                                             p_mi->i_input_id );
    if( !p_input_thread )
    {
        vlc_mutex_unlock( &p_mi->object_lock );
        RAISENULL( "Input does not exist" );
    }

    vlc_mutex_unlock( &p_mi->object_lock );
    return p_input_thread;
}

/*
 * input_state_changed (Private) (input var "state" Callback)
 */
static int
input_state_changed( vlc_object_t * p_this, char const * psz_cmd,
                     vlc_value_t oldval, vlc_value_t newval,
                     void * p_userdata )
{
    libvlc_media_instance_t * p_mi = p_userdata;
    libvlc_event_t event;
    libvlc_event_type_t type = newval.i_int;

    if( strcmp( psz_cmd, "state" ) )
        type = var_GetBool( p_this, "state" );

    switch ( type )
    {
        case END_S:
            libvlc_media_descriptor_set_state( p_mi->p_md, libvlc_NothingSpecial, NULL);
            event.type = libvlc_MediaInstanceReachedEnd;
            break;
        case PAUSE_S:
            libvlc_media_descriptor_set_state( p_mi->p_md, libvlc_Playing, NULL);
            event.type = libvlc_MediaInstancePaused;
            break;
        case PLAYING_S:
            libvlc_media_descriptor_set_state( p_mi->p_md, libvlc_Playing, NULL);
            event.type = libvlc_MediaInstancePlayed;
            break;
        case ERROR_S:
            libvlc_media_descriptor_set_state( p_mi->p_md, libvlc_Error, NULL);
            event.type = libvlc_MediaInstanceReachedEnd; /* Because ERROR_S is buggy */
            break;
        default:
            return VLC_SUCCESS;
    }

    libvlc_event_send( p_mi->p_event_manager, &event );
    return VLC_SUCCESS;
}

/*
 * input_position_changed (Private) (input var "intf-change" Callback)
 */
static int
input_position_changed( vlc_object_t * p_this, char const * psz_cmd,
                     vlc_value_t oldval, vlc_value_t newval,
                     void * p_userdata )
{
    libvlc_media_instance_t * p_mi = p_userdata;
    vlc_value_t val;

    if (!strncmp(psz_cmd, "intf", 4 /* "-change" no need to go further */))
    {
        input_thread_t * p_input = (input_thread_t *)p_this;

        var_Get( p_input, "state", &val );
        if( val.i_int != PLAYING_S )
            return VLC_SUCCESS; /* Don't send the position while stopped */

        var_Get( p_input, "position", &val );
    }
    else
        val.i_time = newval.i_time;

    libvlc_event_t event;
    event.type = libvlc_MediaInstancePositionChanged;
    event.u.media_instance_position_changed.new_position = val.f_float;

    libvlc_event_send( p_mi->p_event_manager, &event );
    return VLC_SUCCESS;
}

/*
 * input_time_changed (Private) (input var "intf-change" Callback)
 */
static int
input_time_changed( vlc_object_t * p_this, char const * psz_cmd,
                     vlc_value_t oldval, vlc_value_t newval,
                     void * p_userdata )
{
    libvlc_media_instance_t * p_mi = p_userdata;
    vlc_value_t val;

    if (!strncmp(psz_cmd, "intf", 4 /* "-change" no need to go further */))
    {
        input_thread_t * p_input = (input_thread_t *)p_this;
    
        var_Get( p_input, "state", &val );
        if( val.i_int != PLAYING_S )
            return VLC_SUCCESS; /* Don't send the position while stopped */

        var_Get( p_input, "time", &val );
    }
    else
        val.i_time = newval.i_time;

    libvlc_event_t event;
    event.type = libvlc_MediaInstanceTimeChanged;
    event.u.media_instance_time_changed.new_time = val.i_time;
    libvlc_event_send( p_mi->p_event_manager, &event );
    return VLC_SUCCESS;
}

/**************************************************************************
 * Create a Media Instance object
 **************************************************************************/
libvlc_media_instance_t *
libvlc_media_instance_new( libvlc_instance_t * p_libvlc_instance,
                           libvlc_exception_t * p_e )
{
    libvlc_media_instance_t * p_mi;

    if( !p_libvlc_instance )
    {
        libvlc_exception_raise( p_e, "invalid libvlc instance" );
        return NULL;
    }

    p_mi = malloc( sizeof(libvlc_media_instance_t) );
    p_mi->p_md = NULL;
    p_mi->drawable = 0;
    p_mi->p_libvlc_instance = p_libvlc_instance;
    p_mi->i_input_id = -1;
    /* refcount strategy:
     * - All items created by _new start with a refcount set to 1
     * - Accessor _release decrease the refcount by 1, if after that
     *   operation the refcount is 0, the object is destroyed.
     * - Accessor _retain increase the refcount by 1 (XXX: to implement) */
    p_mi->i_refcount = 1;
    p_mi->b_own_its_input_thread = VLC_TRUE;
    /* object_lock strategy:
     * - No lock held in constructor
     * - Lock when accessing all variable this lock is held
     * - Lock when attempting to destroy the object the lock is also held */
    vlc_mutex_init( p_mi->p_libvlc_instance->p_libvlc_int,
                    &p_mi->object_lock );
    p_mi->p_event_manager = libvlc_event_manager_new( p_mi,
            p_libvlc_instance, p_e );
    if( libvlc_exception_raised( p_e ) )
    {
        free( p_mi );
        return NULL;
    }
 
    libvlc_event_manager_register_event_type( p_mi->p_event_manager,
            libvlc_MediaInstanceReachedEnd, p_e );
    libvlc_event_manager_register_event_type( p_mi->p_event_manager,
            libvlc_MediaInstancePaused, p_e );
    libvlc_event_manager_register_event_type( p_mi->p_event_manager,
            libvlc_MediaInstancePlayed, p_e );
    libvlc_event_manager_register_event_type( p_mi->p_event_manager,
            libvlc_MediaInstancePositionChanged, p_e );
    libvlc_event_manager_register_event_type( p_mi->p_event_manager,
            libvlc_MediaInstanceTimeChanged, p_e );

    return p_mi;
}

/**************************************************************************
 * Create a Media Instance object with a media descriptor
 **************************************************************************/
libvlc_media_instance_t *
libvlc_media_instance_new_from_media_descriptor(
                                    libvlc_media_descriptor_t * p_md,
                                    libvlc_exception_t *p_e )
{
    libvlc_media_instance_t * p_mi;
    p_mi = libvlc_media_instance_new( p_md->p_libvlc_instance, p_e );

    if( !p_mi )
        return NULL;

    libvlc_media_descriptor_retain( p_md );
    p_mi->p_md = p_md;

    return p_mi;
}

/**************************************************************************
 * Create a new media instance object from an input_thread (Libvlc Internal)
 **************************************************************************/
libvlc_media_instance_t * libvlc_media_instance_new_from_input_thread(
                                   struct libvlc_instance_t *p_libvlc_instance,
                                   input_thread_t *p_input,
                                   libvlc_exception_t *p_e )
{
    libvlc_media_instance_t * p_mi;

    if( !p_input )
    {
        libvlc_exception_raise( p_e, "invalid input thread" );
        return NULL;
    }

    p_mi = libvlc_media_instance_new( p_libvlc_instance, p_e );

    if( !p_mi )
        return NULL;

    p_mi->p_md = libvlc_media_descriptor_new_from_input_item(
                    p_libvlc_instance,
                    input_GetItem( p_input ), p_e );

    if( !p_mi->p_md )
    {
        libvlc_media_instance_destroy( p_mi );
        return NULL;
    }

    p_mi->i_input_id = p_input->i_object_id;
    p_mi->b_own_its_input_thread = VLC_FALSE;

    /* will be released in media_instance_release() */
    vlc_object_yield( p_input );

    /* XXX: Hack as the playlist doesn't yield the input thread we retain
     * the input for the playlist. (see corresponding hack in _release) */
    vlc_object_yield( p_input );

    return p_mi;
}

/**************************************************************************
 * Destroy a Media Instance object (libvlc internal)
 *
 * Warning: No lock held here, but hey, this is internal.
 **************************************************************************/
void libvlc_media_instance_destroy( libvlc_media_instance_t *p_mi )
{
    input_thread_t *p_input_thread;
    libvlc_exception_t p_e;

    libvlc_exception_init( &p_e );

    if( !p_mi )
        return;

    p_input_thread = libvlc_get_input_thread( p_mi, &p_e );

    if( libvlc_exception_raised( &p_e ) )
    {
        libvlc_event_manager_release( p_mi->p_event_manager );
        libvlc_exception_clear( &p_e );
        free( p_mi );
        return; /* no need to worry about no input thread */
    }
    vlc_mutex_destroy( &p_mi->object_lock );

    input_DestroyThread( p_input_thread );

    libvlc_media_descriptor_release( p_mi->p_md );

    free( p_mi );
}

/**************************************************************************
 * Release a Media Instance object
 **************************************************************************/
void libvlc_media_instance_release( libvlc_media_instance_t *p_mi )
{
    if( !p_mi )
        return;

    vlc_mutex_lock( &p_mi->object_lock );
 
    p_mi->i_refcount--;

    if( p_mi->i_refcount > 0 )
    {
        vlc_mutex_unlock( &p_mi->object_lock );
        return;
    }
    vlc_mutex_unlock( &p_mi->object_lock );
    vlc_mutex_destroy( &p_mi->object_lock );

    release_input_thread( p_mi );

    libvlc_event_manager_release( p_mi->p_event_manager );
 
    libvlc_media_descriptor_release( p_mi->p_md );

    free( p_mi );
}

/**************************************************************************
 * Retain a Media Instance object
 **************************************************************************/
void libvlc_media_instance_retain( libvlc_media_instance_t *p_mi )
{
    if( !p_mi )
        return;

    p_mi->i_refcount++;
}

/**************************************************************************
 * Set the Media descriptor associated with the instance
 **************************************************************************/
void libvlc_media_instance_set_media_descriptor(
                            libvlc_media_instance_t *p_mi,
                            libvlc_media_descriptor_t *p_md,
                            libvlc_exception_t *p_e )
{
    (void)p_e;

    if( !p_mi )
        return;

    vlc_mutex_lock( &p_mi->object_lock );

    release_input_thread( p_mi );

    libvlc_media_descriptor_release( p_mi->p_md );

    if( !p_md )
    {
        p_mi->p_md = NULL;
        vlc_mutex_unlock( &p_mi->object_lock );
        return; /* It is ok to pass a NULL md */
    }

    libvlc_media_descriptor_retain( p_md );
    p_mi->p_md = p_md;
 
    /* The policy here is to ignore that we were created using a different
     * libvlc_instance, because we don't really care */
    p_mi->p_libvlc_instance = p_md->p_libvlc_instance;

    vlc_mutex_unlock( &p_mi->object_lock );
}

/**************************************************************************
 * Get the Media descriptor associated with the instance
 **************************************************************************/
libvlc_media_descriptor_t *
libvlc_media_instance_get_media_descriptor(
                            libvlc_media_instance_t *p_mi,
                            libvlc_exception_t *p_e )
{
    (void)p_e;

    if( !p_mi->p_md )
        return NULL;

    libvlc_media_descriptor_retain( p_mi->p_md );
    return p_mi->p_md;
}

/**************************************************************************
 * Get the event Manager
 **************************************************************************/
libvlc_event_manager_t *
libvlc_media_instance_event_manager(
                            libvlc_media_instance_t *p_mi,
                            libvlc_exception_t *p_e )
{
    (void)p_e;

    return p_mi->p_event_manager;
}

/**************************************************************************
 * Play
 **************************************************************************/
void libvlc_media_instance_play( libvlc_media_instance_t *p_mi,
                                 libvlc_exception_t *p_e )
{
    input_thread_t * p_input_thread;

    if( (p_input_thread = libvlc_get_input_thread( p_mi, p_e )) )
    {
        /* A thread alread exists, send it a play message */
        input_Control( p_input_thread, INPUT_SET_STATE, PLAYING_S );
        vlc_object_release( p_input_thread );
        return;
    }

    /* Ignore previous exception */
    libvlc_exception_clear( p_e );

    vlc_mutex_lock( &p_mi->object_lock );
 
    if( !p_mi->p_md )
    {
        libvlc_exception_raise( p_e, "no associated media descriptor" );
        vlc_mutex_unlock( &p_mi->object_lock );
        return;
    }

    p_input_thread = input_CreateThread( p_mi->p_libvlc_instance->p_libvlc_int,
                                         p_mi->p_md->p_input_item );
    p_mi->i_input_id = p_input_thread->i_object_id;

    if( p_mi->drawable )
    {
        vlc_value_t val;
        val.i_int = p_mi->drawable;
        var_Create( p_input_thread, "drawable", VLC_VAR_DOINHERIT );
        var_Set( p_input_thread, "drawable", val );
    }
    var_AddCallback( p_input_thread, "state", input_state_changed, p_mi );
    var_AddCallback( p_input_thread, "seekable", input_state_changed, p_mi );
    var_AddCallback( p_input_thread, "pausable", input_state_changed, p_mi );
    var_AddCallback( p_input_thread, "intf-change", input_position_changed, p_mi );
    var_AddCallback( p_input_thread, "intf-change", input_time_changed, p_mi );

    /* will be released in media_instance_release() */
    vlc_object_yield( p_input_thread );

    vlc_mutex_unlock( &p_mi->object_lock );
}

/**************************************************************************
 * Pause
 **************************************************************************/
void libvlc_media_instance_pause( libvlc_media_instance_t *p_mi,
                                  libvlc_exception_t *p_e )
{
    input_thread_t * p_input_thread = libvlc_get_input_thread( p_mi, p_e );

    if( !p_input_thread )
        return;

    int state = var_GetInteger( p_input_thread, "state" );

    if( state == PLAYING_S )
    {
        if( libvlc_media_instance_can_pause( p_mi, p_e ) )
            input_Control( p_input_thread, INPUT_SET_STATE, PAUSE_S );
        else
            libvlc_media_instance_stop( p_mi, p_e );
    }
    else
        input_Control( p_input_thread, INPUT_SET_STATE, PLAYING_S );

    vlc_object_release( p_input_thread );
}

/**************************************************************************
 * Stop
 **************************************************************************/
void libvlc_media_instance_stop( libvlc_media_instance_t *p_mi,
                                 libvlc_exception_t *p_e )
{
    if( p_mi->b_own_its_input_thread )
        release_input_thread( p_mi ); /* This will stop the input thread */
    else
    {
        input_thread_t * p_input_thread = libvlc_get_input_thread( p_mi, p_e );

        if( !p_input_thread )
            return;

        input_StopThread( p_input_thread );
        vlc_object_release( p_input_thread );
    }
}

/**************************************************************************
 * Set Drawable
 **************************************************************************/
void libvlc_media_instance_set_drawable( libvlc_media_instance_t *p_mi,
                                         libvlc_drawable_t drawable,
                                         libvlc_exception_t *p_e )
{
    p_mi->drawable = drawable;
}

/**************************************************************************
 * Getters for stream information
 **************************************************************************/
libvlc_time_t libvlc_media_instance_get_length(
                             libvlc_media_instance_t *p_mi,
                             libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t val;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e);
    if( !p_input_thread )
        return -1;

    var_Get( p_input_thread, "length", &val );
    vlc_object_release( p_input_thread );

    return (val.i_time+500LL)/1000LL;
}

libvlc_time_t libvlc_media_instance_get_time(
                                   libvlc_media_instance_t *p_mi,
                                   libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t val;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e );
    if( !p_input_thread )
        return -1;

    var_Get( p_input_thread , "time", &val );
    vlc_object_release( p_input_thread );
    return (val.i_time+500LL)/1000LL;
}

void libvlc_media_instance_set_time(
                                 libvlc_media_instance_t *p_mi,
                                 libvlc_time_t time,
                                 libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t value;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e );
    if( !p_input_thread )
        return;

    value.i_time = time*1000LL;
    var_Set( p_input_thread, "time", value );
    vlc_object_release( p_input_thread );
}

void libvlc_media_instance_set_position(
                                libvlc_media_instance_t *p_mi,
                                float position,
                                libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t val;
    val.f_float = position;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e);
    if( !p_input_thread )
        return;

    var_Set( p_input_thread, "position", val );
    vlc_object_release( p_input_thread );
}

float libvlc_media_instance_get_position(
                                 libvlc_media_instance_t *p_mi,
                                 libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t val;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e );
    if( !p_input_thread )
        return -1.0;

    var_Get( p_input_thread, "position", &val );
    vlc_object_release( p_input_thread );

    return val.f_float;
}

void libvlc_media_instance_set_chapter(
                                 libvlc_media_instance_t *p_mi,
                                 int chapter,
                                 libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t val;
    val.i_int = chapter;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e);
    if( !p_input_thread )
        return;

    var_Set( p_input_thread, "chapter", val );
    vlc_object_release( p_input_thread );
}

int libvlc_media_instance_get_chapter(
                                 libvlc_media_instance_t *p_mi,
                                 libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t val;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e );
    if( !p_input_thread )
        return -1.0;

    var_Get( p_input_thread, "chapter", &val );
    vlc_object_release( p_input_thread );

    return val.i_int;
}

int libvlc_media_instance_get_chapter_count(
                                 libvlc_media_instance_t *p_mi,
                                 libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t val;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e );
    if( !p_input_thread )
        return -1.0;

    var_Change( p_input_thread, "chapter", VLC_VAR_CHOICESCOUNT, &val, NULL );
    vlc_object_release( p_input_thread );

    return val.i_int;
}

float libvlc_media_instance_get_fps(
                                 libvlc_media_instance_t *p_mi,
                                 libvlc_exception_t *p_e)
{
    input_thread_t *p_input_thread = libvlc_get_input_thread ( p_mi, p_e );
    double f_fps = 0.0;

    if( p_input_thread )
    {
        if( input_Control( p_input_thread, INPUT_GET_VIDEO_FPS, &f_fps ) )
            f_fps = 0.0;
        vlc_object_release( p_input_thread );
    }
    return f_fps;
}

vlc_bool_t libvlc_media_instance_will_play(
                                 libvlc_media_instance_t *p_mi,
                                 libvlc_exception_t *p_e)
{
    input_thread_t *p_input_thread =
                            libvlc_get_input_thread ( p_mi, p_e);
    if ( !p_input_thread )
        return VLC_FALSE;

    if ( !p_input_thread->b_die && !p_input_thread->b_dead )
    {
        vlc_object_release( p_input_thread );
        return VLC_TRUE;
    }
    vlc_object_release( p_input_thread );
    return VLC_FALSE;
}

void libvlc_media_instance_set_rate(
                                 libvlc_media_instance_t *p_mi,
                                 float rate,
                                 libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t val;

    if( rate <= 0 )
        RAISEVOID( "Rate value is invalid" );

    val.i_int = 1000.0f/rate;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e);
    if ( !p_input_thread )
        return;

    var_Set( p_input_thread, "rate", val );
    vlc_object_release( p_input_thread );
}

float libvlc_media_instance_get_rate(
                                 libvlc_media_instance_t *p_mi,
                                 libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t val;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e);
    if ( !p_input_thread )
        return -1.0;

    var_Get( p_input_thread, "rate", &val );
    vlc_object_release( p_input_thread );

    return (float)1000.0f/val.i_int;
}

libvlc_state_t libvlc_media_instance_get_state(
                                 libvlc_media_instance_t *p_mi,
                                 libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t val;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e );
    if ( !p_input_thread )
    {
        /* We do return the right value, no need to throw an exception */
        if( libvlc_exception_raised( p_e ) )
            libvlc_exception_clear( p_e );
        return libvlc_Stopped;
    }

    var_Get( p_input_thread, "state", &val );
    vlc_object_release( p_input_thread );

    return vlc_to_libvlc_state(val.i_int);
}

vlc_bool_t libvlc_media_instance_is_seekable(
                                 libvlc_media_instance_t *p_mi,
                                 libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t val;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e );
    if ( !p_input_thread )
    {
        /* We do return the right value, no need to throw an exception */
        if( libvlc_exception_raised( p_e ) )
            libvlc_exception_clear( p_e );
        return VLC_FALSE;
    }
    var_Get( p_input_thread, "seekable", &val );
    vlc_object_release( p_input_thread );

    return val.b_bool;
}

vlc_bool_t libvlc_media_instance_can_pause(
                                 libvlc_media_instance_t *p_mi,
                                 libvlc_exception_t *p_e )
{
    input_thread_t *p_input_thread;
    vlc_value_t val;

    p_input_thread = libvlc_get_input_thread ( p_mi, p_e );
    if ( !p_input_thread )
    {
        /* We do return the right value, no need to throw an exception */
        if( libvlc_exception_raised( p_e ) )
            libvlc_exception_clear( p_e );
        return VLC_FALSE;
    }
    var_Get( p_input_thread, "can-pause", &val );
    vlc_object_release( p_input_thread );

    return val.b_bool;
}
