#include "dag.h"


#include "model.h"
#include "redisai.h"
#include "tensor.h"
#include "stats.h"
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdbool.h>
#include "rmutil/alloc.h"
#include "util/arr_rm_alloc.h"
#include "util/dict.h"
#include "util/queue.h"
#include "rmutil/args.h"
#include "run_info.h"


/**
 * Actual method running the DAGRUN Commands in the background
 * thread Called within `RedisAI_Run_ThreadMain`
 */
void *RedisAI_DagRunSession(RedisAI_RunInfo *rinfo) {
  RAI_Error *err = RedisModule_Calloc(1, sizeof(RAI_Error));
  long long rtime;
  int status;
  for (size_t i = 0; i < array_len(rinfo->dagOps); i++)
  {
    RAI_DagOp *currentOp = rinfo->dagOps[i];
    const char *arg_string = RedisModule_StringPtrLen(currentOp->argv[0], NULL);
    currentOp->commandName = RedisModule_Strdup(arg_string);
    if (!strcasecmp(arg_string, "AI.TENSORSET")) {
      RAI_Tensor *t = NULL;
      const int parse_result = RAI_parseTensorSetArgs(
          NULL, currentOp->argv, currentOp->argc, &t, 0, &currentOp->err);
      if (parse_result > 0) {
        const char *key_string =
            RedisModule_StringPtrLen(currentOp->argv[1], NULL);
        AI_dictAdd(rinfo->dagTensorsContext, key_string, t);
        currentOp->result = REDISMODULE_OK;
      } else {
        currentOp->result = REDISMODULE_ERR;
      }
    }
    // if (!strcasecmp(arg_string, "AI.TENSORGET")) {
    //   if (argpos + 1 >= argc) {
    //     RedisModule_WrongArity(ctx);
    //     break;
    //   }
    //   const char *key_string = RedisModule_StringPtrLen(argv[argpos + 1], NULL);
    //   RAI_Tensor *t = NULL;
    //   const int get_result = RAI_getTensorFromLocalContext(
    //       ctx, rinfo->dagTensorsContext, key_string, &t);
    //   if (get_result == REDISMODULE_OK) {
    //     const int parse_result =
    //         RAI_parseTensorGetArgs(ctx, &argv[argpos], argc - argpos, t);
    //     if (parse_result > 0) {
    //       argpos += parse_result - 1;
    //     }
    //   }
    // }
  }
  
  // RedisModule_ReplyWithSimpleString(ctx,"HELLODAG");
  // const long long start = ustime();
  // status = RAI_ModelRun(rinfo->mctx, err);
  // rtime = ustime() - start;

  // size_t noutputs = RAI_ModelRunCtxNumOutputs(rinfo->mctx);
  // for (long long o = 0; o < noutputs; o++) {
  //   RAI_Tensor *tensor = rinfo->mctx->batches[0].outputs[o].tensor;
  //   if (tensor) {
  //     rinfo->mctx->batches[0].outputs[o].tensor =
  //         RAI_TensorGetShallowCopy(tensor);
  //   } else {
  //     rinfo->mctx->batches[0].outputs[o].tensor = NULL;
  //   }
  // }

  // rinfo->result = status;
  // rinfo->err = RedisModule_Calloc(1, sizeof(RAI_Error));
  // rinfo->duration_us = rtime;

  // rinfo->err->code = err->code;
  // if (err->code != RAI_OK) {
  //   rinfo->err->detail = RedisModule_Strdup(err->detail);
  //   rinfo->err->detail_oneline = RedisModule_Strdup(err->detail_oneline);
  // }
  if (rinfo->client != NULL) {
    RedisModule_UnblockClient(rinfo->client, rinfo);
  }
  return NULL;
}

int RedisAI_DagRun_Reply(RedisModuleCtx *ctx, RedisModuleString **argv,
                         int argc) {
  REDISMODULE_NOT_USED(argv);
  REDISMODULE_NOT_USED(argc);
  RedisAI_RunInfo *rinfo = RedisModule_GetBlockedClientPrivateData(ctx);
  RedisModule_ReplyWithArray(ctx, REDISMODULE_POSTPONED_ARRAY_LEN);
  for (size_t i = 0; i < array_len(rinfo->dagOps); i++)
  {
    RAI_DagOp *currentOp = rinfo->dagOps[i];
    if (!strcasecmp(currentOp->commandName, "AI.TENSORSET")) {
      rinfo->dagReplyLength++;
      if(currentOp->result==REDISMODULE_ERR){
        RedisModule_ReplyWithError(ctx, currentOp->err->detail_oneline);
        RAI_ClearError(currentOp->err);
      }else{
        RedisModule_ReplyWithSimpleString(ctx, "OK");
      }
    }
    
    /* code */
  }
  
  AI_dictIterator *persist_iter =
      AI_dictGetSafeIterator(rinfo->dagTensorsPersistentContext);
  AI_dictEntry *persist_entry = AI_dictNext(persist_iter);
  while (persist_entry) {
    const char *persist_key_name = AI_dictGetKey(persist_entry);
    AI_dictEntry *tensor_entry =
        AI_dictFind(rinfo->dagTensorsContext, persist_key_name);
    if (tensor_entry) {
      RAI_Tensor *tensor = AI_dictGetVal(tensor_entry);
      RedisModuleKey *key;
      RedisModuleString *tensor_keyname = RedisModule_CreateString(
          ctx, persist_key_name, strlen(persist_key_name));
      const int status = RAI_OpenKey_Tensor(
          ctx, tensor_keyname, &key, REDISMODULE_READ | REDISMODULE_WRITE);
      if (status == REDISMODULE_ERR) {
        RedisModule_ReplyWithError(ctx, "ERR could not save tensor");
        rinfo->dagReplyLength++;
      } else {
        if (RedisModule_ModuleTypeSetValue(key, RedisAI_TensorType, tensor) !=
            REDISMODULE_OK) {
          RedisModule_ReplyWithError(ctx, "ERR could not save tensor");
          rinfo->dagReplyLength++;
        }
      }
      RedisModule_CloseKey(key);
      RedisAI_ReplicateTensorSet(ctx, tensor_keyname, tensor);
      // TODO: free Tensor
    } else {
      RedisModule_ReplyWithError(
          ctx, "ERR specified persistent key that was not used on DAG");
      rinfo->dagReplyLength++;
    }
    persist_entry = AI_dictNext(persist_iter);
  }
  AI_dictReleaseIterator(persist_iter);
  RedisModule_ReplySetArrayLength(ctx, rinfo->dagReplyLength);
  return REDISMODULE_OK;
}

/**
 * DAGRUN Building Block to parse [LOAD <nkeys> key1 key2... ]
 */
int RAI_parseDAGLoadArgs(RedisModuleCtx *ctx, RedisModuleString **argv,
                         int argc, AI_dict **localContextDict,
                         const char *chaining_operator) {
  if (argc < 3) {
    RedisModule_WrongArity(ctx);
    return -1;
  }

  long long n_keys;
  const int retval = RedisModule_StringToLongLong(argv[1], &n_keys);
  if (retval != REDISMODULE_OK || n_keys <= 0) {
    RedisModule_ReplyWithError(
        ctx, "ERR invalid or negative value found in number of keys to LOAD");
    return -1;
  }

  int number_loaded_keys = 0;
  int separator_flag = 0;
  size_t argpos = 2;
  for (; (argpos <= argc - 1) && (number_loaded_keys < n_keys); argpos++) {
    const char *arg_string = RedisModule_StringPtrLen(argv[argpos], NULL);
    if (!strcasecmp(arg_string, chaining_operator)) {
      separator_flag = 1;
      break;
    } else {
      RAI_Tensor *t;
      RedisModuleKey *key;
      const int status = RAI_GetTensorFromKeyspace(ctx, argv[argpos], &key, &t,
                                                   REDISMODULE_READ);
      if (status == REDISMODULE_ERR) {
        RedisModule_Log(
            ctx, "warning",
            "on DAGRUN's LOAD could not load tensor %s from keyspace",
            arg_string);
        return -1;
      }
      AI_dictAdd(*localContextDict, arg_string, t);
      number_loaded_keys++;
    }
  }
  if (number_loaded_keys != n_keys) {
    RedisModule_WrongArity(ctx);
    return -1;
  }
  return argpos;
}

/**
 * DAGRUN Building Block to parse [PERSIST <nkeys> key1 key2... ]
 */
int RAI_parseDAGPersistArgs(RedisModuleCtx *ctx, RedisModuleString **argv,
                            int argc, AI_dict **localContextDict,
                            const char *chaining_operator) {
  if (argc < 3) {
    RedisModule_WrongArity(ctx);
    return -1;
  }

  long long n_keys;
  const int retval = RedisModule_StringToLongLong(argv[1], &n_keys);
  if (retval != REDISMODULE_OK || n_keys <= 0) {
    RedisModule_ReplyWithError(
        ctx, "ERR invalid or negative value found in number of keys to LOAD");
    return -1;
  }

  int number_loaded_keys = 0;
  int separator_flag = 0;
  size_t argpos = 2;
  for (; (argpos <= argc - 1) && (number_loaded_keys < n_keys); argpos++) {
    const char *arg_string = RedisModule_StringPtrLen(argv[argpos], NULL);
    if (!strcasecmp(arg_string, chaining_operator)) {
      separator_flag = 1;
      break;
    } else {
      AI_dictAdd(*localContextDict, arg_string, 1);
      number_loaded_keys++;
    }
  }
  if (number_loaded_keys != n_keys) {
    RedisModule_WrongArity(ctx);
    return -1;
  }
  return argpos;
}

// int RAI_background(){
  
//     if (!strcasecmp(arg_string, "AI.MODELRUN")) {
//       reply_length++;
//       if (argpos + 1 >= argc) {
//         RedisModule_WrongArity(ctx);
//         break;
//       }

//       RAI_Model *mto;
//       RedisModuleKey *modelKey;
//       const int status = RAI_GetModelFromKeyspace(
//           ctx, argv[argpos + 1], &modelKey, &mto, REDISMODULE_READ);
//       if (status == REDISMODULE_ERR) {
//         return REDISMODULE_ERR;
//       }

//       RedisModule_RetainString(NULL, argv[1]);
//       rinfo->runkey = argv[1];
//       rinfo->mctx = RAI_ModelRunCtxCreate(mto);
//       rinfo->sctx = NULL;
//       rinfo->outkeys = NULL;
//       rinfo->err = NULL;

//       const int parse_result = RedisAI_Parse_ModelRun_RedisCommand(
//           ctx,&argv[argpos], argc - argpos, &rinfo, &mto, 1,
//           &(rinfo->dagTensorsContext), 1, "|>");
//       if (parse_result > 0) {
//         argpos += parse_result - 1;

//       }

//       // TODO: parse and process modelrun
//     }

// }