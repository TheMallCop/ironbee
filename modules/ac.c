/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 * @brief IronBee - AhoCorasick Matcher Module
 *
 * This module adds an AhoCorasick based matcher named "ac".
 *
 * @author Pablo Rincon <pablo.rincon.crespo@gmail.com>
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>

#include <ironbee/ahocorasick.h>
#include <ironbee/bytestr.h>
#include <ironbee/cfgmap.h>
#include <ironbee/debug.h>
#include <ironbee/engine.h>
#include <ironbee/module.h>
#include <ironbee/mpool.h>
#include <ironbee/operator.h>
#include <ironbee/provider.h>
#include <ironbee/types.h>

/* Define the module name as well as a string version of it. */
#define MODULE_NAME        ac
#define MODULE_NAME_STR    IB_XSTRINGIFY(MODULE_NAME)

/* Informational extra data.. version of this module (should be better to
 * register it with the module itself) */
#define AC_MAJOR           0
#define AC_MINOR           1
#define AC_DATE            20110812

typedef struct modac_cfg_t modac_cfg_t;
typedef struct modac_cpatt_t modac_cpatt_t;

/* Define the public module symbol. */
IB_MODULE_DECLARE();

/**
 * @internal
 * Module Configuration Structure.
 */
struct modac_cfg_t {
/* @todo: implement limits on ahocorasick to support this options:
    match_limit and match_limit_recursion */
    ib_num_t       match_limit;           /**< Match limit */
    ib_num_t       match_limit_recursion; /**< Match recursion depth limit */
};

/* Instantiate a module global configuration. */
static modac_cfg_t modac_global_cfg;

/**
 * @internal
 * Internal representation of AC compiled patterns.
 */
struct modac_provider_data_t {
    ib_ac_t *ac_tree;                 /**< The AC tree */
};

/* Instantiate a module global configuration. */
typedef struct modac_provider_data_t modac_provider_data_t;


/* -- Matcher Interface -- */

/**
 * Add a pattern to the patterns of the matcher given a pattern and
 * callback + extra arg
 *
 * @param mpr matcher provider
 * @param patterns pointer to the pattern container (ie: an AC tree)
 * @param patt the pattern to be added
 * @param callback the callback to register with the given pattern
 * @param arg the extra argument to pass to the callback
 * @param errptr a pointer reference to point where an error ocur
 * @param erroffset a pointer holding the offset of the error
 *
 * @return status of the operation
 */
static ib_status_t modac_add_pattern_ex(ib_provider_inst_t *mpi,
                                        void *patterns,
                                        const char *patt,
                                        ib_void_fn_t callback,
                                        void *arg,
                                        const char **errptr,
                                        int *erroffset)
{
    IB_FTRACE_INIT(modac_add_pattern_ex);
    ib_status_t rc;
    ib_ac_t *ac_tree = (ib_ac_t *)((modac_provider_data_t*)mpi->data)->ac_tree;

    /* If the ac_tree doesn't exist, create it before adding the pattern */
    if (ac_tree == NULL) {
        rc = ib_ac_create(&ac_tree, 0, mpi->mp);
        if (rc != IB_OK || ac_tree == NULL) {
            ib_log_error(mpi->pr->ib, 4,
                         "Unable to create the AC tree at modac");
            IB_FTRACE_RET_STATUS(rc);
        }
        ((modac_provider_data_t*)mpi->data)->ac_tree = ac_tree;
    }

    rc = ib_ac_add_pattern(ac_tree, patt, (ib_ac_callback_t)callback, arg, 0);

    if (rc == IB_OK) {
        ib_log_debug(mpi->pr->ib, 4, "pattern %s added to the AC tree %x", patt,
                     ac_tree);
    }
    else {
        ib_log_error(mpi->pr->ib, 4, "Failed to load pattern %s to the AC tree %x",
                     patt, ac_tree);
    }

    IB_FTRACE_RET_STATUS(rc);
}

/**
 * Initialize a provider instance with the given data
 *
 * @param mpi provider instance
 * @param extra data
 *
 * @return status of the operation
 */
static ib_status_t modac_provider_instance_init(ib_provider_inst_t *mpi,
                                                void *data)
{
    IB_FTRACE_INIT(modac_provider_instance_init);
    ib_status_t rc;
    modac_provider_data_t *dt;

    dt = (modac_provider_data_t *) ib_mpool_calloc(mpi->mp, 1,
                                         sizeof(modac_provider_data_t));
    if (dt == NULL) {
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    mpi->data = (void *)dt;
    rc = ib_ac_create(&dt->ac_tree, 0, mpi->mp);

    if (rc != IB_OK) {
        ib_log_error(mpi->pr->ib, 4, "Unable to create the AC tree at modac");
    }

    IB_FTRACE_RET_STATUS(rc);
}

/**
 * Match against the AC tree
 *
 * @param mpi provider instance
 * @param flags extra flags
 * @param data the data to search in
 * @param dlen length of the the data to search in
 *
 * @return status of the operation
 */
static ib_status_t modac_match(ib_provider_inst_t *mpi,
                                 ib_flags_t flags,
                                 const uint8_t *data,
                                 size_t dlen, void *ctx)
{
    IB_FTRACE_INIT(modac_match);
    modac_provider_data_t *dt = mpi->data;

    if (dt == NULL) {
        IB_FTRACE_RET_STATUS(IB_EINVAL);
    }

    ib_log_debug(mpi->pr->ib, 4, "Matching AGAINST AC tree %x",
                     dt->ac_tree);


    ib_ac_t *ac_tree = dt->ac_tree;

    ib_ac_context_t *ac_mctx = (ib_ac_context_t *)ctx;

    ib_ac_reset_ctx(ac_mctx, ac_tree);

    /* Let's perform the search. Content is consumed in just one call */
    ib_status_t rc = ib_ac_consume(ac_mctx,
                                   (const char *)data,
                                   dlen,
                                   IB_AC_FLAG_CONSUME_DOLIST |
                                   IB_AC_FLAG_CONSUME_MATCHALL |
                                   IB_AC_FLAG_CONSUME_DOCALLBACK,
                                   mpi->mp);

    IB_FTRACE_RET_STATUS(rc);
}

static ib_status_t modac_compile(ib_provider_t *mpr,
                                   ib_mpool_t *pool,
                                   void *pcpatt,
                                   const char *patt,
                                   const char **errptr,
                                   int *erroffset)
{
    IB_FTRACE_INIT(modac_compile);
    IB_FTRACE_RET_STATUS(IB_ENOTIMPL);
}

static ib_status_t modac_match_compiled(ib_provider_t *mpr,
                                          void *cpatt,
                                          ib_flags_t flags,
                                          const uint8_t *data,
                                          size_t dlen, void *ctx)
{
    IB_FTRACE_INIT(modac_match_compiled);
    IB_FTRACE_RET_STATUS(IB_ENOTIMPL);
}

static ib_status_t modac_add_pattern(ib_provider_inst_t *pi,
                                       void *cpatt)
{
    IB_FTRACE_INIT(modac_add);
    IB_FTRACE_RET_STATUS(IB_ENOTIMPL);
}

static IB_PROVIDER_IFACE_TYPE(matcher) modac_matcher_iface = {
    IB_PROVIDER_IFACE_HEADER_DEFAULTS,

    /* Provider Interface */
    modac_compile,
    modac_match_compiled,

    /* Provider Instance Interface */
    modac_add_pattern,
    modac_add_pattern_ex,
    modac_match
};

static void nop_ac_match(ib_ac_t *orig,
                         ib_ac_char_t *pattern,
                         size_t pattern_len,
                         void* userdata,
                         size_t offset,
                         size_t relative_offset)
{
    /* Nop. */
}

/**
 * @internal
 * @brief Read the given file into memory and return the malloced buffer.
 * @param[in] filename Filename to read.
 * @param[in,out] buffer Character buffer pointer that will be malloced
 *                and must be free'ed by the caller.
 * @param[in,out] len The length of buffer, which will also be resized.
 */
static ib_status_t readfile(const char* filename, char **buffer)
{
    IB_FTRACE_INIT(readfile);

    int fd = open(filename, O_RDONLY);
    int rc;
    struct stat fd_stat;
    ssize_t len; /**< Length of the file. */
    ssize_t bytes_read;
    ssize_t total_bytes_read;

    if (fd < 0) {
        fprintf(stderr,
                "Failed to open pattern file %s - %s",
                filename,
                strerror(errno));
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    rc = fstat(fd, &fd_stat);

    if (rc == -1) {
        fprintf(stderr,
                "Failed to stat file %s - %s", filename, strerror(errno));
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* Protect the user from building a tree from a 1GB file of patterns. */
    if (fd_stat.st_size > 1024000000) {
        fprintf(stderr,
                "Refusing to parse file %s because it is too large.",
                filename);
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    /* If conversion from off_t to ssize_t is required, it happens here. */
    len = fd_stat.st_size;

    *buffer = malloc(len);

    total_bytes_read = 0;

    do {
        bytes_read = read(fd,
                          (*buffer)+total_bytes_read,
                          len - total_bytes_read);

        if (bytes_read < 0) {
            free(*buffer);
            *buffer = NULL;
            len = 0;
            IB_FTRACE_RET_STATUS(IB_EALLOC);
        }

        total_bytes_read += bytes_read;
    } while(total_bytes_read < len);

    if (*buffer==NULL) {
        close(fd);
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    IB_FTRACE_RET_STATUS(IB_OK);
}

static ib_status_t pmf_operator_create(ib_mpool_t *pool,
                                       const char *pattern_file,
                                       ib_operator_inst_t *op_inst)
{
    IB_FTRACE_INIT(pmf_operator_create);

    ib_status_t rc;

    ib_ac_t *ac;

    char* file = NULL;
    char* line = NULL;

    rc = readfile(pattern_file, &file);

    if (rc != IB_OK) {
        if (file != NULL) {
            free(file);
        }

        IB_FTRACE_RET_STATUS(rc);
    }

    rc = ib_ac_create(&ac, 0, pool);

    if (rc != IB_OK) {
        free(file);
        IB_FTRACE_RET_STATUS(rc);
    }

    for (line=strtok(file, "\n"); line != NULL; line=strtok(NULL, "\n")) {
        rc = ib_ac_add_pattern (ac, line, &nop_ac_match, NULL, 0);

        if (rc != IB_OK) {
            free(file);
            IB_FTRACE_RET_STATUS(rc);
        }
    }


    rc = ib_ac_build_links(ac);

    if (rc != IB_OK) {
        free(file);
        IB_FTRACE_RET_STATUS(rc);
    }

    op_inst->data = ac;

    free(file);
    IB_FTRACE_RET_STATUS(IB_OK);
}

static ib_status_t pm_operator_create(ib_mpool_t *pool,
                                      const char *pattern,
                                      ib_operator_inst_t *op_inst)
{
    IB_FTRACE_INIT(pm_operator_create);

    ib_status_t rc;

    ib_ac_t *ac;

    size_t tok_buffer_sz = strlen(pattern)+1;
    char* tok_buffer = malloc(tok_buffer_sz);
    char* tok;

    if (tok_buffer == NULL ) {
        IB_FTRACE_RET_STATUS(IB_EALLOC);
    }

    memcpy(tok_buffer, pattern, tok_buffer_sz);

    rc = ib_ac_create(&ac, 0, pool);

    if (rc != IB_OK) {
        free(tok_buffer);
        IB_FTRACE_RET_STATUS(rc);
    }

    for (tok = strtok(tok_buffer, " "); tok != NULL; tok = strtok(NULL, " "))
    {
        if (strlen(tok) > 0 ) {
            rc = ib_ac_add_pattern (ac, tok, &nop_ac_match, NULL, 0);

            if (rc != IB_OK) {
                free(tok_buffer);
                IB_FTRACE_RET_STATUS(rc);
            }
        }
    }

    rc = ib_ac_build_links(ac);

    if (rc != IB_OK) {
        free(tok_buffer);
        IB_FTRACE_RET_STATUS(rc);
    }

    op_inst->data = ac;

    free(tok_buffer);
    IB_FTRACE_RET_STATUS(IB_OK);
}

static ib_status_t pm_operator_execute(ib_engine_t *ib,
                                       ib_tx_t *tx,
                                       void *data,
                                       ib_field_t *field,
                                       ib_num_t *result)
{
    IB_FTRACE_INIT(pm_operator_execute);

    ib_ac_t *ac = (ib_ac_t*)data;
    ib_ac_context_t ac_ctx;
    ib_status_t rc;

    char* subject;
    size_t subject_len;
    ib_bytestr_t* bytestr;

    if (field->type == IB_FTYPE_NULSTR) {
        subject = ib_field_value_nulstr(field);
        subject_len = strlen(subject);
    }
    else if (field->type == IB_FTYPE_BYTESTR) {
        bytestr = ib_field_value_bytestr(field);
        subject_len = ib_bytestr_length(bytestr);
        subject = (char*) ib_bytestr_ptr(bytestr);
    }
    else {
        return IB_EALLOC;
    }

    ib_ac_init_ctx(&ac_ctx, ac);

    rc = ib_ac_consume(&ac_ctx, subject, subject_len, 0, tx->mp);

    if (rc == IB_ENOENT) {
        *result = 0;
        IB_FTRACE_RET_STATUS(IB_OK);
    } else if (rc == IB_OK) {
        *result = (ac_ctx.match_cnt > 0) ? 1 : 0;
        IB_FTRACE_RET_STATUS(IB_OK);
    }

    IB_FTRACE_RET_STATUS(rc);
}

static ib_status_t pm_operator_destroy(ib_operator_inst_t *op_inst)
{
    IB_FTRACE_INIT(pm_operator_destroy);

    /* Nop. */

    /* No callback required. Allocations are out of the IB memory pool. */

    IB_FTRACE_RET_STATUS(IB_OK);
}

/* -- Module Routines -- */

static ib_status_t modac_init(ib_engine_t *ib,
                                ib_module_t *m)
{
    IB_FTRACE_INIT(modac_init);
    ib_status_t rc;

    /* Register as a matcher provider. */
    rc = ib_provider_register(ib,
                              IB_PROVIDER_TYPE_MATCHER,
                              MODULE_NAME_STR,
                              NULL,
                              &modac_matcher_iface,
                              modac_provider_instance_init);
    if (rc != IB_OK) {
        ib_log_error(ib, 3,
                     MODULE_NAME_STR ": Error registering ac matcher provider: "
                     "%d", rc);
        IB_FTRACE_RET_STATUS(IB_OK);
    }

    ib_operator_register(ib, "@pm", 0,
                         &pm_operator_create,
                         &pm_operator_destroy,
                         &pm_operator_execute);
    ib_operator_register(ib, "@pmf", 0,
                         &pmf_operator_create,
                         &pm_operator_destroy,
                         &pm_operator_execute);

    ib_log_debug(ib, 4, "AC Status: compiled=\"%d.%d %s\" AC Matcher"
                        " registered", AC_MAJOR, AC_MINOR, IB_XSTRINGIFY(AC_DATE));

    IB_FTRACE_RET_STATUS(IB_OK);
}

static IB_CFGMAP_INIT_STRUCTURE(modac_config_map) = {
    IB_CFGMAP_INIT_ENTRY(
        MODULE_NAME_STR ".match_limit",
        IB_FTYPE_NUM,
        &modac_global_cfg,
        match_limit,
        5000
    ),
    IB_CFGMAP_INIT_ENTRY(
        MODULE_NAME_STR ".match_limit_recursion",
        IB_FTYPE_NUM,
        &modac_global_cfg,
        match_limit_recursion,
        5000
    ),
    IB_CFGMAP_INIT_LAST
};

/**
 * @internal
 * Module structure.
 *
 * This structure defines some metadata, config data and various functions.
 */
IB_MODULE_INIT(
    IB_MODULE_HEADER_DEFAULTS,            /**< Default metadata */
    MODULE_NAME_STR,                      /**< Module name */
    IB_MODULE_CONFIG(&modac_global_cfg),  /**< Global config data */
    modac_config_map,                     /**< Configuration field map */
    NULL,                                 /**< Config directive map */
    modac_init,                           /**< Initialize function */
    NULL,                                 /**< Finish function */
    NULL,                                 /**< Context init function */
    NULL                                  /**< Context fini function */
);

