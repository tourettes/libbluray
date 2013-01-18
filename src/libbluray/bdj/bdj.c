/*
 * This file is part of libbluray
 * Copyright (C) 2010  William Hahne
 * Copyright (C) 2012  Petri Hintukainen <phintuka@users.sourceforge.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.s
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include "bdj.h"

#include "bdj_private.h"
#include "bdjo_parser.h"
#include "bdj_util.h"
#include "common.h"
#include "libbluray/register.h"
#include "file/file.h"
#include "file/dl.h"
#include "util/strutl.h"
#include "util/macro.h"
#include "util/logging.h"
#include "libbluray/bdnav/bdid_parse.h"
#include "libbluray/bdj/native/register_native.h"

#include <jni.h>
#include <stdlib.h>
#include <string.h>

// stat
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

typedef jint (JNICALL * fptr_JNI_CreateJavaVM) (JavaVM **pvm, void **penv,void *args);

static void *_load_jvm(void)
{
    const char* java_home = getenv("JAVA_HOME"); // FIXME: should probably search multiple directories
    if (java_home == NULL) {
        BD_DEBUG(DBG_BDJ | DBG_CRIT, "JAVA_HOME not set, trying default locations\n");

        void *h = dl_dlopen("libjvm", NULL);
        if (h) {
            return h;
        }

        java_home = "/usr/lib/jvm/default-java/";
    }

#ifdef WIN32
    char* path = str_printf("%s/jre/bin/server/jvm", java_home);
#else	//	#ifdef WIN32
    char* path = str_printf("%s/jre/lib/%s/server/libjvm", java_home, JAVA_ARCH);
#endif	//	#ifdef WIN32

    return dl_dlopen(path, NULL);
}

static const char *_find_libbluray_jar(void)
{
    // pre-defined search paths for libbluray.jar
    static const char * const jar_paths[] = {
#ifdef WIN32
        "" BDJ_JARFILE,
#else
        "/usr/lib/libbluray/" BDJ_JARFILE,
        "/usr/share/libbluray/" BDJ_JARFILE,
#endif
    };

    static const char *classpath = NULL;

    struct stat sb;
    unsigned i;

    // check if overriding the classpath
    if (!classpath) {
        classpath = getenv("LIBBLURAY_CP");
    }
    if (classpath) {
        return classpath;
    }

    BD_DEBUG(DBG_BDJ, "LIBBLURAY_CP not set, searching for "BDJ_JARFILE" ...\n");

    // check pre-defined directories
    for (i = 0; i < sizeof(jar_paths) / sizeof(jar_paths[0]); i++) {
        BD_DEBUG(DBG_BDJ, "Checking %s ...\n", jar_paths[i]);
        if (!stat(jar_paths[i], &sb)) {
            classpath = jar_paths[i];
            BD_DEBUG(DBG_BDJ, "using %s\n", classpath);
            return classpath;
        }
    }

    classpath = BDJ_CLASSPATH;
    BD_DEBUG(DBG_BDJ | DBG_CRIT, BDJ_JARFILE" not found.\n");
    return classpath;
}

static int _bdj_init(BDJAVA *bdjava, JNIEnv *env)
{
    if (!bdj_register_native_methods(env)) {
        BD_DEBUG(DBG_BDJ | DBG_CRIT, "Couldn't register native methods.\n");
    }

    // initialize class org.videolan.Libbluray
    jclass init_class;
    jmethodID init_id;
    if (!bdj_get_method(env, &init_class, &init_id,
                        "org/videolan/Libbluray", "init", "(JLjava/lang/String;)V")) {
        return 0;
    }

    char* id_path = str_printf("%s/CERTIFICATE/id.bdmv", bdjava->path);
    BDID_DATA *id  = bdid_parse(id_path);
    jlong param_bdjava_ptr = (jlong)(intptr_t) bdjava;
    jstring param_disc_id = (*env)->NewStringUTF(env,
                                                 id ? id->disc_id : "00000000000000000000000000000000");
    (*env)->CallStaticVoidMethod(env, init_class, init_id,
                                 param_bdjava_ptr, param_disc_id);

    if ((*env)->ExceptionOccurred(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }

    (*env)->DeleteLocalRef(env, init_class);
    (*env)->DeleteLocalRef(env, param_disc_id);

    free(id_path);
    bdid_free(&id);

    return 1;
}

BDJAVA* bdj_open(const char *path, struct bluray *bd,
                 struct bd_registers_s *registers, struct indx_root_s *index,
                 bdj_overlay_cb osd_cb, struct bd_argb_buffer_s *buf)
{
    BD_DEBUG(DBG_BDJ, "bdj_open()\n");

    // first load the jvm using dlopen
    void* jvm_lib = _load_jvm();

    if (!jvm_lib) {
        BD_DEBUG(DBG_BDJ | DBG_CRIT, "Wasn't able to load JVM\n");
        return NULL;
    }

    fptr_JNI_CreateJavaVM JNI_CreateJavaVM_fp = (fptr_JNI_CreateJavaVM)dl_dlsym(jvm_lib, "JNI_CreateJavaVM");

    if (JNI_CreateJavaVM_fp == NULL) {
        dl_dlclose(jvm_lib);
        BD_DEBUG(DBG_BDJ | DBG_CRIT, "Couldn't find symbol JNI_CreateJavaVM.\n");
        return NULL;
    }

    BDJAVA* bdjava = calloc(1, sizeof(BDJAVA));
    bdjava->bd = bd;
    bdjava->reg = registers;
    bdjava->index = index;
    bdjava->path = path;
    bdjava->h_libjvm = jvm_lib;

    bdjava->osd_cb = osd_cb;
    bdjava->buf = buf;

    JavaVMInitArgs args;

    // determine classpath
    //char* classpath_opt = str_printf("-Djava.class.path=%s", classpath);
    char* classpath_opt = str_printf("-Xbootclasspath/a:%s", _find_libbluray_jar());

    // determine bluray.vfs.root
    char* vfs_opt;
    vfs_opt = str_printf("-Dbluray.vfs.root=%s", path);

    JavaVMOption* option = calloc(1, sizeof(JavaVMOption) * 9);
    int n = 0;
    option[n++].optionString = classpath_opt;
    option[n++].optionString = vfs_opt;

    args.version = JNI_VERSION_1_4;
    args.nOptions = n;
    args.options = option;
    args.ignoreUnrecognized = JNI_FALSE; // don't ignore unrecognized options

    JNIEnv* env = NULL;
    int result = JNI_CreateJavaVM_fp(&bdjava->jvm, (void**) &env, &args);
    free(option);
    free(classpath_opt);
    free(vfs_opt);

    if (result != JNI_OK || !env) {
        bdj_close(bdjava);
        BD_DEBUG(DBG_BDJ | DBG_CRIT, "Failed to create new Java VM.\n");
        return NULL;
    }

    if (!_bdj_init(bdjava, env)) {
        bdj_close(bdjava);
        return NULL;
    }

    return bdjava;
}

int bdj_start(BDJAVA *bdjava, unsigned title)
{
    JNIEnv* env;
    int attach = 0;
    jboolean status = JNI_FALSE;
    jclass loader_class;
    jmethodID load_id;

    if (!bdjava) {
        return BDJ_ERROR;
    }

    BD_DEBUG(DBG_BDJ, "bdj_start(%d)\n", title);

    if ((*bdjava->jvm)->GetEnv(bdjava->jvm, (void**)&env, JNI_VERSION_1_4) != JNI_OK) {
        (*bdjava->jvm)->AttachCurrentThread(bdjava->jvm, (void**)&env, NULL);
        attach = 1;
    }

    if (bdj_get_method(env, &loader_class, &load_id,
                       "org/videolan/BDJLoader", "load", "(I)Z")) {
        status = (*env)->CallStaticBooleanMethod(env, loader_class, load_id, (jint)title);

        if ((*env)->ExceptionOccurred(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        }

        (*env)->DeleteLocalRef(env, loader_class);
    }

    if (attach) {
        (*bdjava->jvm)->DetachCurrentThread(bdjava->jvm);
    }

    return (status == JNI_TRUE) ? BDJ_SUCCESS : BDJ_ERROR;
}

int bdj_stop(BDJAVA *bdjava)
{
    JNIEnv *env;
    int attach;
    jboolean status = JNI_FALSE;
    jclass loader_class;
    jmethodID unload_id;

    if (!bdjava) {
        return BDJ_ERROR;
    }

    BD_DEBUG(DBG_BDJ, "bdj_stop()\n");

    if ((*bdjava->jvm)->GetEnv(bdjava->jvm, (void**)&env, JNI_VERSION_1_4) != JNI_OK) {
        (*bdjava->jvm)->AttachCurrentThread(bdjava->jvm, (void**)&env, NULL);
        attach = 1;
    }

    if (bdj_get_method(env, &loader_class, &unload_id,
                       "org/videolan/BDJLoader", "unload", "()Z")) {
        status = (*env)->CallStaticBooleanMethod(env, loader_class, unload_id);

        if ((*env)->ExceptionOccurred(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        }

        (*env)->DeleteLocalRef(env, loader_class);
    }

    if (attach) {
        (*bdjava->jvm)->DetachCurrentThread(bdjava->jvm);
    }

    return (status == JNI_TRUE) ? BDJ_SUCCESS : BDJ_ERROR;
}

void bdj_close(BDJAVA *bdjava)
{
    JNIEnv *env;
    int attach;
    jclass shutdown_class;
    jmethodID shutdown_id;

    if (!bdjava) {
        return;
    }

    BD_DEBUG(DBG_BDJ, "bdj_close()\n");

    if (bdjava->jvm) {
        if ((*bdjava->jvm)->GetEnv(bdjava->jvm, (void**)&env, JNI_VERSION_1_4) != JNI_OK) {
            (*bdjava->jvm)->AttachCurrentThread(bdjava->jvm, (void**)&env, NULL);
            attach = 1;
        }

        if (bdj_get_method(env, &shutdown_class, &shutdown_id,
                           "org/videolan/Libbluray", "shutdown", "()V")) {
            (*env)->CallStaticVoidMethod(env, shutdown_class, shutdown_id);

            if ((*env)->ExceptionOccurred(env)) {
                (*env)->ExceptionDescribe(env);
                (*env)->ExceptionClear(env);
            }

            (*env)->DeleteLocalRef(env, shutdown_class);
        }

        if (attach) {
            (*bdjava->jvm)->DetachCurrentThread(bdjava->jvm);
        }

        (*bdjava->jvm)->DestroyJavaVM(bdjava->jvm);
    }

    if (bdjava->h_libjvm) {
        dl_dlclose(bdjava->h_libjvm);
    }

    X_FREE(bdjava);
}

void bdj_process_event(BDJAVA *bdjava, unsigned ev, unsigned param)
{
    static const char * const ev_name[] = {
        "NONE",
        "CHAPTER",
        "PLAYITEM",
        "ANGLE",
        "SUBTITLE",
        "PIP",
        "END_OF_PLAYLIST",
        "PTS",
        "VK_KEY",
    };

    JNIEnv* env;
    int attach = 0;
    jclass event_class;
    jmethodID event_id;

    if (!bdjava) {
        return;
    }

    BD_DEBUG(DBG_BDJ, "bdj_process_event(%s,%d)\n", ev_name[ev], param);

    if ((*bdjava->jvm)->GetEnv(bdjava->jvm, (void**)&env, JNI_VERSION_1_4) != JNI_OK) {
        (*bdjava->jvm)->AttachCurrentThread(bdjava->jvm, (void**)&env, NULL);
        attach = 1;
    }

    if (bdj_get_method(env, &event_class, &event_id,
                       "org/videolan/Libbluray", "processEvent", "(II)V")) {
        (*env)->CallStaticVoidMethod(env, event_class, event_id, ev, param);

        if ((*env)->ExceptionOccurred(env)) {
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        }

        (*env)->DeleteLocalRef(env, event_class);
    }

    if (attach) {
        (*bdjava->jvm)->DetachCurrentThread(bdjava->jvm);
    }
}
