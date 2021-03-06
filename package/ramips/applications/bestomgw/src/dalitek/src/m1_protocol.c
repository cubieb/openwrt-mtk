#include <sys/time.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>

#include "m1_protocol.h"
#include "socket_server.h"
#include "buf_manage.h"
#include "m1_project.h"
#include "sql_backup.h"
#include "m1_common_log.h"
#include "m1_device.h"

/*Macro**********************************************************************************************************/
#define SQL_HISTORY_DEL      1
#define M1_PROTOCOL_DEBUG    1
#define HEAD_LEN             3
#define AP_HEARTBEAT_HANDLE  1
/*Private function***********************************************************************************************/
static int AP_report_data_handle(payload_t data);
static int APP_read_handle(payload_t data);
static int APP_write_handle(payload_t data);
static int M1_write_to_AP(cJSON* data, sqlite3* db);
static int APP_echo_dev_info_handle(payload_t data);
static int APP_req_added_dev_info_handle(payload_t data);
static int APP_net_control(payload_t data);
static int M1_report_dev_info(payload_t data);
static int M1_report_ap_info(payload_t data);
static int AP_report_dev_handle(payload_t data);
static int AP_report_ap_handle(payload_t data);
static int common_operate(payload_t data);
static int common_rsp(rsp_data_t data);
static int ap_heartbeat_handle(payload_t data);
static int common_rsp_handle(payload_t data);
static int create_sql_table(void);
static int app_change_device_name(payload_t data);
static uint8_t hex_to_uint8(int h);
static void check_offline_dev(sqlite3*db);
static void delete_client_db(void);
/*variable******************************************************************************************************/
extern pthread_mutex_t mutex_lock;
sqlite3* db = NULL;
const char* db_path = "/bin/dev_info.db";
const char* sql_back_path = "/bin/sql_restore.sh";
fifo_t dev_data_fifo;
fifo_t link_exec_fifo;
fifo_t msg_rd_fifo;
fifo_t msg_wt_fifo;
fifo_t client_delete_fifo;
fifo_t tx_fifo;
/*优先级队列*/
PNode head;
static uint32_t dev_data_buf[256];
static uint32_t link_exec_buf[256];
static uint32_t client_delete_buf[10];

void m1_protocol_init(void)
{
    create_sql_table();
    M1_LOG_DEBUG("threadsafe:%d\n",sqlite3_threadsafe());
    fifo_init(&dev_data_fifo, dev_data_buf, 256);
    /*linkage execution fifo*/
    fifo_init(&link_exec_fifo, link_exec_buf, 256);
    /*delete cleint*/
    /*linkage execution fifo*/
    fifo_init(&client_delete_fifo, client_delete_buf, 10);

    Init_PQueue(&head);
    /*初始化接收buf*/
    client_block_init();
}

void data_handle(m1_package_t* package)
{
    M1_LOG_DEBUG("data_handle\n");
    int rc, ret;
    int pduType;
    uint32_t* msg = NULL;
    cJSON* rootJson = NULL;
    cJSON* pduJson = NULL;
    cJSON* pduTypeJson = NULL;
    cJSON* snJson = NULL;
    cJSON* pduDataJson = NULL;

    payload_t pdu;
    rsp_data_t rspData;

    rc = M1_PROTOCOL_NO_RSP;
    M1_LOG_DEBUG("Rx message:%s\n",package->data);
    rootJson = cJSON_Parse(package->data);
    if(NULL == rootJson){
        M1_LOG_ERROR("rootJson null\n");
        goto Finish;   
    }
    pduJson = cJSON_GetObjectItem(rootJson, "pdu");
    if(NULL == pduJson){
        M1_LOG_ERROR("pdu null\n");
        goto Finish;
    }
    pduTypeJson = cJSON_GetObjectItem(pduJson, "pduType");
    if(NULL == pduTypeJson){
        M1_LOG_ERROR("pduType null\n");
        goto Finish;
    }
    pduType = pduTypeJson->valueint;
    rspData.pduType = pduType;

    snJson = cJSON_GetObjectItem(rootJson, "sn");
    if(NULL == snJson){
        M1_LOG_ERROR("sn null\n");
        goto Finish;
    }
    rspData.sn = snJson->valueint;

    pduDataJson = cJSON_GetObjectItem(pduJson, "devData");
    if(NULL == pduDataJson){
        M1_LOG_INFO("devData null”\n");
    }

    /*打开读数据库*/
    rc = sql_open();
    if( rc != SQLITE_OK){  
        M1_LOG_ERROR( "Can't open database\n");  
        goto Finish;
    }else{  
        M1_LOG_DEBUG( "Opened database successfully\n");  
    }

    /*pdu*/ 
    pdu.clientFd = package->clientFd;
    pdu.sn = snJson->valueint;
    pdu.db = db;
    pdu.pdu = pduDataJson;

    rspData.clientFd = package->clientFd;
    M1_LOG_DEBUG("pduType:%x\n",pduType);

    switch(pduType){
        case TYPE_DEV_READ: APP_read_handle(pdu); break;
        case TYPE_REQ_ADDED_INFO: APP_req_added_dev_info_handle(pdu); break;
        case TYPE_DEV_NET_CONTROL: rc = APP_net_control(pdu); break;
        case TYPE_REQ_AP_INFO: M1_report_ap_info(pdu); break;
        case TYPE_REQ_DEV_INFO: M1_report_dev_info(pdu); break;
        case TYPE_COMMON_RSP: common_rsp_handle(pdu);rc = M1_PROTOCOL_NO_RSP;break;
        case TYPE_REQ_SCEN_INFO: rc = app_req_scenario(pdu);break;
        case TYPE_REQ_LINK_INFO: rc = app_req_linkage(pdu);break;
        case TYPE_REQ_DISTRICT_INFO: rc = app_req_district(pdu); break;
        case TYPE_REQ_SCEN_NAME_INFO: rc = app_req_scenario_name(pdu);break;
        case TYPE_REQ_ACCOUNT_INFO: rc = app_req_account_info_handle(pdu);break;
        case TYPE_REQ_ACCOUNT_CONFIG_INFO: rc = app_req_account_config_handle(pdu);break;
        case TYPE_GET_PORJECT_NUMBER: rc = app_get_project_info(pdu); break;
        case TYPE_REQ_DIS_SCEN_NAME: rc = app_req_dis_scen_name(pdu); break;
        case TYPE_REQ_DIS_NAME: rc = app_req_dis_name(pdu); break;
        case TYPE_REQ_DIS_DEV: rc = app_req_dis_dev(pdu); break;
        case TYPE_GET_PROJECT_INFO: rc = app_get_project_config(pdu);break;
        case TYPE_APP_CONFIRM_PROJECT: rc = app_confirm_project(pdu);break;
        case TYPE_APP_EXEC_SCEN: rc = app_exec_scenario(pdu);break;
        case TYPE_DEBUG_INFO: debug_switch(pduDataJson->valuestring);break;
        /*write*/
        case TYPE_REPORT_DATA: rc = AP_report_data_handle(pdu); break;
        case TYPE_DEV_WRITE: rc = M1_write_to_AP(rootJson, db);/*APP_write_handle(pdu);*/break;
        case TYPE_ECHO_DEV_INFO: rc = APP_echo_dev_info_handle(pdu); break;
        case TYPE_AP_REPORT_DEV_INFO: rc = AP_report_dev_handle(pdu); break;
        case TYPE_AP_REPORT_AP_INFO: rc = AP_report_ap_handle(pdu); break;
        case TYPE_CREATE_LINKAGE: rc = linkage_msg_handle(pdu);break;
        case TYPE_CREATE_SCENARIO: rc = scenario_create_handle(pdu);break;
        case TYPE_CREATE_DISTRICT: rc = district_create_handle(pdu);break;
        case TYPE_SCENARIO_ALARM: rc = scenario_alarm_create_handle(pdu);break;
        case TYPE_COMMON_OPERATE: rc = common_operate(pdu);break;
        case TYPE_AP_HEARTBEAT_INFO: rc = ap_heartbeat_handle(pdu);break;
        case TYPE_LINK_ENABLE_SET: rc = app_linkage_enable(pdu);break;
        case TYPE_APP_LOGIN: rc = user_login_handle(pdu);break;
        case TYPE_SEND_ACCOUNT_CONFIG_INFO: rc = app_account_config_handle(pdu);break;
        case TYPE_APP_CREATE_PROJECT: rc = app_create_project(pdu);break;
        case TYPE_PROJECT_KEY_CHANGE: rc = app_change_project_key(pdu);break;
        case TYPE_PROJECT_INFO_CHANGE:rc = app_change_project_config(pdu);break;
        case TYPE_APP_CHANGE_DEV_NAME: rc = app_change_device_name(pdu);break;
        case TYPE_APP_USER_KEY_CHANGE: rc = app_change_user_key(pdu);break;
        case TYPE_APP_DOWNLOAD_TESTING_INFO: rc = app_download_testing_to_ap(rootJson,db); break;
        case TYPE_AP_UPLOAD_TESTING_INFO: rc = ap_upload_testing_to_app(rootJson,db);break;

        default: M1_LOG_ERROR("pdu type not match\n"); rc = M1_PROTOCOL_FAILED;break;
    }

    if(rc != M1_PROTOCOL_NO_RSP){
        if(rc == M1_PROTOCOL_OK)
            rspData.result = RSP_OK;
        else
            rspData.result = RSP_FAILED;
        common_rsp(rspData);
    }
    check_offline_dev(db);

    delete_client_db();

    sql_close();

    Finish:
#if SQL_HISTORY_DEL
    // if(sql_backup() != M1_PROTOCOL_OK){
    //     M1_LOG_ERROR( "sql_backup failed\n");
    // }
#endif
    cJSON_Delete(rootJson);
    linkage_task();
}

static int common_rsp_handle(payload_t data)
{
    cJSON* resultJson = NULL;
    M1_LOG_DEBUG("common_rsp_handle\n");
    if(data.pdu == NULL) return M1_PROTOCOL_FAILED;

    resultJson = cJSON_GetObjectItem(data.pdu, "result");
    M1_LOG_DEBUG("result:%d\n",resultJson->valueint);
}

/*AP report device data to M1*/
static int AP_report_data_handle(payload_t data)
{
    int id;
    int i,j;
    int number1, number2;
    int rc,ret = M1_PROTOCOL_OK;
    char* time = (char*)malloc(30);
    char* errorMsg = NULL;
    char* sql = "select ID from param_table order by ID desc limit 1";
//#if (!SQL_HISTORY_DEL)
    char* sql_1 = (char*)malloc(300);
    sqlite3_stmt* stmt_1 = NULL;
//#endif
    /*sqlite3*/
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;
    /*Json*/
    cJSON* devDataJson = NULL;
    cJSON* devNameJson = NULL;
    cJSON* devIdJson = NULL;
    cJSON* paramJson = NULL;
    cJSON* paramDataJson = NULL;
    cJSON* typeJson = NULL;
    cJSON* valueJson = NULL;

    db = data.db;   
    M1_LOG_DEBUG("AP_report_data_handle\n");
    if(data.pdu == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;   
    }
    /*关闭写同步*/
    if(sqlite3_exec(db,"PRAGMA synchronous = OFF; ",0,0,0) != SQLITE_OK){
        M1_LOG_ERROR("PRAGMA synchronous = OFF falied\n");
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }

    /*获取系统当前时间*/
    getNowTime(time);
    /*添加update/insert/delete监察*/
    rc = sqlite3_update_hook(db, trigger_cb, "AP_report_data_handle");
    if(rc){
        M1_LOG_ERROR( "sqlite3_update_hook falied: %s\n", sqlite3_errmsg(db));  
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    if(sqlite3_exec(db, "BEGIN", NULL, NULL, &errorMsg)==SQLITE_OK){
        M1_LOG_DEBUG("BEGIN\n");
        id = sql_id(db, sql);
        M1_LOG_DEBUG("id:%d\n",id);
        sql = "insert into param_table(ID, DEV_NAME,DEV_ID,TYPE,VALUE,TIME) values(?,?,?,?,?,?);";
        if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
            M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));   
            ret = M1_PROTOCOL_FAILED;
            goto Finish; 
        }

        number1 = cJSON_GetArraySize(data.pdu);
        M1_LOG_DEBUG("number1:%d\n",number1);
        for(i = 0; i < number1; i++){
            devDataJson = cJSON_GetArrayItem(data.pdu, i);
            if(devDataJson == NULL){
                ret =  M1_PROTOCOL_FAILED;  
                goto Finish;    
            }
            devNameJson = cJSON_GetObjectItem(devDataJson, "devName");
            if(devNameJson == NULL){
                ret =  M1_PROTOCOL_FAILED;  
                goto Finish;    
            }
            M1_LOG_DEBUG("devName:%s\n",devNameJson->valuestring);
            devIdJson = cJSON_GetObjectItem(devDataJson, "devId");
            if(devIdJson == NULL){
                ret =  M1_PROTOCOL_FAILED;  
                goto Finish;    
            }
            M1_LOG_DEBUG("devId:%s\n",devIdJson->valuestring);
            paramJson = cJSON_GetObjectItem(devDataJson, "param");
            if(paramJson == NULL){
                ret =  M1_PROTOCOL_FAILED;  
                goto Finish;    
            }
            number2 = cJSON_GetArraySize(paramJson);
            M1_LOG_DEBUG(" number2:%d\n",number2);
            for(j = 0; j < number2; j++){
                paramDataJson = cJSON_GetArrayItem(paramJson, j);
                if(paramDataJson == NULL){
                    ret =  M1_PROTOCOL_FAILED;  
                    goto Finish;    
                }   
                typeJson = cJSON_GetObjectItem(paramDataJson, "type");
                if(typeJson == NULL){
                    ret =  M1_PROTOCOL_FAILED;  
                    goto Finish;    
                }
                M1_LOG_DEBUG("  type:%d\n",typeJson->valueint);
                valueJson = cJSON_GetObjectItem(paramDataJson, "value");
                if(valueJson == NULL){
                    ret =  M1_PROTOCOL_FAILED;  
                    goto Finish;    
                }
                M1_LOG_DEBUG("  value:%d\n",valueJson->valueint);
                if(typeJson->valueint == 16404){
                    M1_LOG_DEBUG("type:%05d,value:%d,dev_id:%s\n",typeJson->valueint,valueJson->valueint,devIdJson->valuestring);    
                }
                M1_LOG_DEBUG("AP data insert\n");
                /*先删除*/
                sprintf(sql_1,"delete from param_table where DEV_ID = \"%s\" and TYPE = %05d;",devIdJson->valuestring,typeJson->valueint);
                M1_LOG_DEBUG("sql_1:%s\n",sql_1);
                sqlite3_finalize(stmt_1);
                if(sqlite3_prepare_v2(db, sql_1, strlen(sql_1), &stmt_1, NULL) != SQLITE_OK){
                    M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                    ret = M1_PROTOCOL_FAILED;
                    goto Finish; 
                }
                
                rc = thread_sqlite3_step(&stmt_1, db);
                /*再插入*/
                sqlite3_reset(stmt); 
                sqlite3_clear_bindings(stmt);
                sqlite3_bind_int(stmt, 1, id);
                id++;
                sqlite3_bind_text(stmt, 2,  devNameJson->valuestring, -1, NULL);
                sqlite3_bind_text(stmt, 3, devIdJson->valuestring, -1, NULL);
                sqlite3_bind_int(stmt, 4,typeJson->valueint);
                sqlite3_bind_int(stmt, 5, valueJson->valueint);
                sqlite3_bind_text(stmt, 6,  time, -1, NULL);

                thread_sqlite3_step(&stmt, db);

            }
        }
        if(sqlite3_exec(db, "COMMIT", NULL, NULL, &errorMsg) == SQLITE_OK){
            M1_LOG_DEBUG("END\n");
            sqlite3_free(errorMsg);
        }else{
            M1_LOG_DEBUG("ROLLBACK\n");
            if(sqlite3_exec(db, "ROLLBACK", NULL, NULL, &errorMsg) == SQLITE_OK){
                M1_LOG_DEBUG("ROLLBACK OK\n");
                sqlite3_free(errorMsg);
            }else{
                M1_LOG_ERROR("ROLLBACK FALIED\n");
            }
        }
 
    }

    Finish:
//#if (!SQL_HISTORY_DEL)
    free(sql_1);
    sqlite3_finalize(stmt_1);
//#endif
    sqlite3_finalize(stmt);

    trigger_cb_handle(db);
    free(time);
    return ret;
}

/*AP report device information to M1*/
static int AP_report_dev_handle(payload_t data)
{
    int i;
    int id;
    int number;
    int rc, ret = M1_PROTOCOL_OK;
    char* time = (char*)malloc(30);
    char* errorMsg = NULL;
    char* sql = NULL;
    char* sql_1 = (char*)malloc(300);
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;
    sqlite3_stmt* stmt_1 = NULL;
    cJSON* apIdJson = NULL;
    cJSON* apNameJson = NULL;
    cJSON* pIdJson = NULL;
    cJSON* devJson = NULL;
    cJSON* paramDataJson = NULL;    
    cJSON* idJson = NULL;    
    cJSON* nameJson = NULL;   

    M1_LOG_DEBUG("AP_report_dev_handle\n");
    if(data.pdu == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    } 

    getNowTime(time);
    /*获取数据库*/
    db = data.db;
    /*关闭写同步*/
    if(sqlite3_exec(db,"PRAGMA synchronous = OFF; ",0,0,0) != SQLITE_OK){
        M1_LOG_ERROR("PRAGMA synchronous = OFF falied\n");
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }

    apIdJson = cJSON_GetObjectItem(data.pdu,"apId");
    M1_LOG_DEBUG("APId:%s\n",apIdJson->valuestring);
    apNameJson = cJSON_GetObjectItem(data.pdu,"apName");
    M1_LOG_DEBUG("APName:%s\n",apNameJson->valuestring);
    devJson = cJSON_GetObjectItem(data.pdu,"dev");
    number = cJSON_GetArraySize(devJson); 

    /*事物开始*/
    if(sqlite3_exec(db, "BEGIN", NULL, NULL, &errorMsg)==SQLITE_OK){
        M1_LOG_DEBUG("BEGIN\n");
        sql = "insert into all_dev(ID, DEV_NAME, DEV_ID, AP_ID, PID, ADDED, NET, STATUS, ACCOUNT,TIME) values(?,?,?,?,?,?,?,?,?,?);";
        if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
            M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
            ret = M1_PROTOCOL_FAILED;
            goto Finish; 
        }

        for(i = 0; i< number; i++){
            paramDataJson = cJSON_GetArrayItem(devJson, i);
            if(paramDataJson == NULL){
                ret = M1_PROTOCOL_FAILED;
                goto Finish;
            }
            idJson = cJSON_GetObjectItem(paramDataJson, "devId");
            if(idJson == NULL){
                ret = M1_PROTOCOL_FAILED;
                goto Finish;
            }
            M1_LOG_DEBUG("devId:%s\n", idJson->valuestring);
            nameJson = cJSON_GetObjectItem(paramDataJson, "devName");
            if(nameJson == NULL){
                ret = M1_PROTOCOL_FAILED;
                goto Finish;
            }
            M1_LOG_DEBUG("devName:%s\n", nameJson->valuestring);
            pIdJson = cJSON_GetObjectItem(paramDataJson, "pId");
            if(pIdJson == NULL){
                ret = M1_PROTOCOL_FAILED;
                goto Finish;
            }
            M1_LOG_DEBUG("pId:%05d\n", pIdJson->valueint);
            /*判断该设备是否存在*/
            sprintf(sql_1,"select ID from all_dev where DEV_ID = \"%s\" and AP_ID = \"%s\";",idJson->valuestring, apIdJson->valuestring);
            /*get id*/
            sqlite3_finalize(stmt_1); 
            if(sqlite3_prepare_v2(db, sql_1, strlen(sql_1), &stmt_1, NULL) != SQLITE_OK){
                M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                ret = M1_PROTOCOL_FAILED;
                goto Finish; 
            }    

            rc = thread_sqlite3_step(&stmt_1, db);
            if(rc == SQLITE_ROW){
                id = (sqlite3_column_int(stmt_1, 0));
            }else{
                sprintf(sql_1,"select ID from all_dev order by ID desc limit 1");
                id = sql_id(db, sql_1);
                sqlite3_reset(stmt);
                sqlite3_clear_bindings(stmt); 
                sqlite3_bind_int(stmt, 1, id);
                sqlite3_bind_text(stmt, 2,  nameJson->valuestring, -1, NULL);
                sqlite3_bind_text(stmt, 3, idJson->valuestring, -1, NULL);
                sqlite3_bind_text(stmt, 4,apIdJson->valuestring, -1, NULL);
                sqlite3_bind_int(stmt, 5, pIdJson->valueint);
                sqlite3_bind_int(stmt, 6, 0);
                sqlite3_bind_int(stmt, 7, 1);
                sqlite3_bind_text(stmt, 8,"ON", -1, NULL);
                sqlite3_bind_text(stmt, 9,  "Dalitek", -1, NULL);
                sqlite3_bind_text(stmt, 10,  time, -1, NULL);
                rc = thread_sqlite3_step(&stmt,db);
            }
        }

        if(sqlite3_exec(db, "COMMIT", NULL, NULL, &errorMsg) == SQLITE_OK){
            M1_LOG_DEBUG("END\n");
        }else{
            M1_LOG_DEBUG("ROLLBACK\n");
            if(sqlite3_exec(db, "ROLLBACK", NULL, NULL, &errorMsg) == SQLITE_OK){
                M1_LOG_DEBUG("ROLLBACK OK\n");
                sqlite3_free(errorMsg);
            }else{
                M1_LOG_ERROR("ROLLBACK FALIED\n");
            }
        }
    }else{
        M1_LOG_ERROR("errorMsg:");
    }   

    Finish:
    free(time);
    free(sql_1);
    sqlite3_free(errorMsg);
    sqlite3_finalize(stmt);
    sqlite3_finalize(stmt_1); 

    return ret;  
}

/*AP report information to M1*/
static int AP_report_ap_handle(payload_t data)
{
    int id;
    int rc,ret = M1_PROTOCOL_OK;
    char* sql = NULL; 
    char* sql_1 = (char*)malloc(300);
    char* sql_2 = (char*)malloc(300);
    char* time = (char*)malloc(30);
    char* errorMsg = NULL;
    cJSON* pIdJson = NULL;
    cJSON* apIdJson = NULL;
    cJSON* apNameJson = NULL;
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;
    sqlite3_stmt* stmt_1 = NULL;
    sqlite3_stmt* stmt_2 = NULL;

    M1_LOG_DEBUG("AP_report_ap_handle\n");
    if(data.pdu == NULL){
        ret = M1_PROTOCOL_FAILED;  
        goto Finish;
    }

    getNowTime(time);
    /*获取数据库*/
    db = data.db;

    pIdJson = cJSON_GetObjectItem(data.pdu,"pId");
    if(pIdJson == NULL){
        ret = M1_PROTOCOL_FAILED;  
        goto Finish;
    }
    M1_LOG_DEBUG("pId:%05d\n",pIdJson->valueint);
    apIdJson = cJSON_GetObjectItem(data.pdu,"apId");
    if(apIdJson == NULL){
        ret = M1_PROTOCOL_FAILED;  
        goto Finish;
    }
    M1_LOG_DEBUG("APId:%s\n",apIdJson->valuestring);
    apNameJson = cJSON_GetObjectItem(data.pdu,"apName");
    if(apNameJson == NULL){
        ret = M1_PROTOCOL_FAILED;  
        goto Finish;
    }
    M1_LOG_DEBUG("APName:%s\n",apNameJson->valuestring);
    if(sqlite3_exec(db, "BEGIN", NULL, NULL, &errorMsg)==SQLITE_OK){
        M1_LOG_DEBUG("BEGIN\n");
        /*update clientFd*/
        sprintf(sql_1, "select ID from conn_info where AP_ID  = \"%s\"", apIdJson->valuestring);
        rc = sql_row_number(db, sql_1);
        M1_LOG_DEBUG("rc:%d\n",rc);
        if(rc > 0){
            sprintf(sql_1, "update conn_info set CLIENT_FD = %d where AP_ID  = \"%s\"", data.clientFd, apIdJson->valuestring);
            M1_LOG_DEBUG("%s\n",sql_1);
        }else{
            sql = "select ID from conn_info order by ID desc limit 1";
            id = sql_id(db, sql);
            sprintf(sql_1, " insert into conn_info(ID, AP_ID, CLIENT_FD) values(%d,\"%s\",%d);", id, apIdJson->valuestring, data.clientFd);
            M1_LOG_DEBUG("%s\n",sql_1);
        }
        rc = sql_exec(db, sql_1);
        M1_LOG_DEBUG("exec:%s\n",rc == SQLITE_DONE ? "SQLITE_DONE": rc == SQLITE_ROW ? "SQLITE_ROW" : "SQLITE_ERROR");
        if(rc == SQLITE_ERROR){
            ret = M1_PROTOCOL_FAILED;  
            if(sqlite3_exec(db, "COMMIT", NULL, NULL, &errorMsg) == SQLITE_OK){
                M1_LOG_DEBUG("END\n");
            }else{
                M1_LOG_DEBUG("ROLLBACK\n");
                if(sqlite3_exec(db, "ROLLBACK", NULL, NULL, &errorMsg) == SQLITE_OK){
                    M1_LOG_DEBUG("ROLLBACK OK\n");
                    sqlite3_free(errorMsg);
                }else{
                    M1_LOG_ERROR("ROLLBACK FALIED\n");
                }
            }
            goto Finish;       
        };

        sprintf(sql_1,"select ID from all_dev where DEV_ID = \"%s\";",apIdJson->valuestring);
        sqlite3_finalize(stmt_1);
        if(sqlite3_prepare_v2(db, sql_1, strlen(sql_1), &stmt_1, NULL) != SQLITE_OK){
            M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
            ret = M1_PROTOCOL_FAILED;
            goto Finish; 
        }
        rc =  thread_sqlite3_step(&stmt_1, db);
        if(rc != SQLITE_ROW){
            /*insert sql*/
            sql = "insert into all_dev(ID, DEV_NAME, DEV_ID, AP_ID, PID, ADDED, NET, STATUS, ACCOUNT,TIME) values(?,?,?,?,?,?,?,?,?,?);";
            M1_LOG_DEBUG("string:%s\n",sql);
            if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
                M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                ret = M1_PROTOCOL_FAILED;
                goto Finish; 
            }
            /*判断该设备是否存在*/   
            sprintf(sql_1,"select ID from all_dev order by ID desc limit 1");
            id = sql_id(db, sql_1);

            sqlite3_bind_int(stmt, 1, id);
            sqlite3_bind_text(stmt, 2,  apNameJson->valuestring, -1, NULL);
            sqlite3_bind_text(stmt, 3, apIdJson->valuestring, -1, NULL);
            sqlite3_bind_text(stmt, 4,apIdJson->valuestring, -1, NULL);
            sqlite3_bind_int(stmt, 5, pIdJson->valueint);
            sqlite3_bind_int(stmt, 6, 0);
            sqlite3_bind_int(stmt, 7, 1);
            sqlite3_bind_text(stmt, 8,  "ON", -1, NULL);
            sqlite3_bind_text(stmt, 9,  "Dalitek", -1, NULL);
            sqlite3_bind_text(stmt, 10,  time, -1, NULL);
            rc = thread_sqlite3_step(&stmt,db);
            /*插入AP在线状态*/
            sprintf(sql_1,"select ID from param_table order by ID desc limit 1");
            M1_LOG_DEBUG("string:%s\n",sql_1);
            id = sql_id(db, sql_1);
            /*插入AP在线信息*/
            sprintf(sql_2,"insert into param_table(ID, DEV_ID, DEV_NAME, TYPE, VALUE, TIME) values(?,?,?,?,?,?);");
            M1_LOG_DEBUG("string:%s\n",sql_2);
            if(sqlite3_prepare_v2(db, sql_2, strlen(sql_2), &stmt_2, NULL) != SQLITE_OK){
                M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                ret = M1_PROTOCOL_FAILED;
                goto Finish; 
            }
            sqlite3_bind_int(stmt_2, 1, id);
            sqlite3_bind_text(stmt_2, 2,  apIdJson->valuestring, -1, NULL);
            sqlite3_bind_text(stmt_2, 3,  apNameJson->valuestring, -1, NULL);
            sqlite3_bind_int(stmt_2, 4, 16404);
            sqlite3_bind_int(stmt_2, 5, 1);
            sqlite3_bind_text(stmt_2, 6, time, -1, NULL);
            thread_sqlite3_step(&stmt_2, db);
        }
        if(sqlite3_exec(db, "COMMIT", NULL, NULL, &errorMsg) == SQLITE_OK){
           M1_LOG_DEBUG("END\n");
        }else{
            M1_LOG_DEBUG("ROLLBACK\n");
            if(sqlite3_exec(db, "ROLLBACK", NULL, NULL, &errorMsg) == SQLITE_OK){
                M1_LOG_DEBUG("ROLLBACK OK\n");
                sqlite3_free(errorMsg);
            }else{
                M1_LOG_ERROR("ROLLBACK FALIED\n");
            }
        }
    }else{
        M1_LOG_ERROR("errorMsg:");
    }    
    
    Finish:
    free(time);
    free(sql_1);
    free(sql_2);
    sqlite3_free(errorMsg);
    sqlite3_finalize(stmt);
    sqlite3_finalize(stmt_1);
    sqlite3_finalize(stmt_2);

    return ret;  
}

static int APP_read_handle(payload_t data)
{   
    /*read json*/
    int i,j;
    int number1,number2,row_n;
    int pduType = TYPE_REPORT_DATA;
    int rc, ret = M1_PROTOCOL_OK;
    char * devName = NULL;
    char* pId = NULL;
    int value = 0;
    int clientFd = 0;
    char* dev_id = NULL;
    char* sql = (char*)malloc(500);
    cJSON* devDataJson = NULL;
    cJSON* devIdJson = NULL;
    cJSON* paramTypeJson = NULL;
    cJSON* paramJson = NULL;
    cJSON * pJsonRoot = NULL; 
    cJSON * pduJsonObject = NULL;
    cJSON * devDataJsonArray = NULL;
    cJSON * devDataObject= NULL;
    cJSON * devArray = NULL;
    cJSON*  devObject = NULL;
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL, *stmt_1 = NULL;

    M1_LOG_DEBUG("APP_read_handle\n");
    if(data.pdu == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }

    clientFd = data.clientFd;
    db = data.db;

    /*get sql data json*/
    pJsonRoot = cJSON_CreateObject();
    if(NULL == pJsonRoot)
    {
        M1_LOG_DEBUG("pJsonRoot NULL\n");
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    cJSON_AddNumberToObject(pJsonRoot, "sn", data.sn);
    cJSON_AddStringToObject(pJsonRoot, "version", "1.0");
    cJSON_AddNumberToObject(pJsonRoot, "netFlag", 1);
    cJSON_AddNumberToObject(pJsonRoot, "cmdType", 1);
    /*create pdu object*/
    pduJsonObject = cJSON_CreateObject();
    if(NULL == pduJsonObject)
    {
        // create object faild, exit
        cJSON_Delete(pduJsonObject);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    /*add pdu to root*/
    cJSON_AddItemToObject(pJsonRoot, "pdu", pduJsonObject);
    /*add pdu type to pdu object*/
    cJSON_AddNumberToObject(pduJsonObject, "pduType", pduType);
    /*create devData array*/
    devDataJsonArray = cJSON_CreateArray();
    if(NULL == devDataJsonArray)
    {
        cJSON_Delete(devDataJsonArray);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    /*add devData array to pdu pbject*/
    cJSON_AddItemToObject(pduJsonObject, "devData", devDataJsonArray);

    number1 = cJSON_GetArraySize(data.pdu);
    M1_LOG_DEBUG("number1:%d\n",number1);

    for(i = 0; i < number1; i++){
        /*read json*/
        devDataJson = cJSON_GetArrayItem(data.pdu, i);
        devIdJson = cJSON_GetObjectItem(devDataJson, "devId");
        M1_LOG_DEBUG("devId:%s\n",devIdJson->valuestring);
        dev_id = devIdJson->valuestring;
        paramTypeJson = cJSON_GetObjectItem(devDataJson, "paramType");
        number2 = cJSON_GetArraySize(paramTypeJson);
        /*get sql data json*/
        sprintf(sql, "select DEV_NAME from all_dev where DEV_ID  = \"%s\" order by ID desc limit 1;", dev_id);
        M1_LOG_DEBUG("%s\n", sql);

        devDataObject = cJSON_CreateObject();
        if(NULL == devDataObject)
        {
            // create object faild, exit
            M1_LOG_ERROR("devDataObject NULL\n");
            cJSON_Delete(devDataObject);
            ret = M1_PROTOCOL_FAILED;
            goto Finish;
        }
        cJSON_AddItemToArray(devDataJsonArray, devDataObject);
        cJSON_AddStringToObject(devDataObject, "devId", dev_id);
        /*取出devName*/
        sqlite3_finalize(stmt);
        if(sqlite3_prepare_v2(db, sql, strlen(sql),&stmt, NULL) != SQLITE_OK){
            M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
            ret = M1_PROTOCOL_FAILED;
            goto Finish; 
        }
        rc = thread_sqlite3_step(&stmt, db);
        if(rc == SQLITE_ROW){
            devName = sqlite3_column_text(stmt,0);
            if(devName == NULL){
                ret = M1_PROTOCOL_FAILED;
                goto Finish;       
            }
            cJSON_AddStringToObject(devDataObject, "devName", devName);
        }else{
            M1_LOG_WARN("devName not exit");
            continue;
        }
        /*添加PID*/
        sprintf(sql, "select PID from all_dev where DEV_ID  = \"%s\" order by ID desc limit 1;", dev_id);
        M1_LOG_DEBUG("%s\n", sql);
        sqlite3_finalize(stmt);
        if(sqlite3_prepare_v2(db, sql, strlen(sql),&stmt, NULL) != SQLITE_OK){
            M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
            ret = M1_PROTOCOL_FAILED;
            goto Finish; 
        }
        rc = thread_sqlite3_step(&stmt, db);
        if(rc == SQLITE_ROW){
            pId = sqlite3_column_text(stmt,0);
            if(pId == NULL){
                ret = M1_PROTOCOL_FAILED;
                goto Finish;   
            }
            cJSON_AddStringToObject(devDataObject, "pId", pId);
        }else{
            M1_LOG_WARN("pId not exit");
            continue;   
        }

        devArray = cJSON_CreateArray();
        if(NULL == devArray)
        {
            M1_LOG_ERROR("devArry NULL\n");
            cJSON_Delete(devArray);
            ret = M1_PROTOCOL_FAILED;
            goto Finish;
        }
        /*add devData array to pdu pbject*/
        cJSON_AddItemToObject(devDataObject, "param", devArray);
    
        for(j = 0; j < number2; j++){
            /*read json*/
            paramJson = cJSON_GetArrayItem(paramTypeJson, j);
            /*get sql data json*/
            sprintf(sql, "select VALUE from param_table where DEV_ID  = \"%s\" and TYPE = %05d order by ID desc limit 1;", dev_id, paramJson->valueint);
            M1_LOG_DEBUG("%s\n", sql);
     
            devObject = cJSON_CreateObject();
            if(NULL == devObject)
            {
                M1_LOG_ERROR("devObject NULL\n");
                cJSON_Delete(devObject);
                ret = M1_PROTOCOL_FAILED;
                goto Finish;
            }
            cJSON_AddItemToArray(devArray, devObject); 

            sqlite3_finalize(stmt_1);
            if(sqlite3_prepare_v2(db, sql, strlen(sql),&stmt_1, NULL) != SQLITE_OK){
                M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                ret = M1_PROTOCOL_FAILED;
                goto Finish; 
            }
            rc = thread_sqlite3_step(&stmt_1,db);
            if(rc == SQLITE_ROW){
                value = sqlite3_column_int(stmt_1,0);
                cJSON_AddNumberToObject(devObject, "type", paramJson->valueint);
                cJSON_AddNumberToObject(devObject, "value", value);
            }else{
                M1_LOG_DEBUG("value not exit");
                continue;
            }
       
        }
    }

    char * p = cJSON_PrintUnformatted(pJsonRoot);
    
    if(NULL == p)
    {    
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }

    M1_LOG_DEBUG("string:%s\n",p);
    socketSeverSend((uint8*)p, strlen(p), clientFd);
    Finish:
    free(sql);
    sqlite3_finalize(stmt);
    sqlite3_finalize(stmt_1);
    cJSON_Delete(pJsonRoot);

    return ret;
}

static int M1_write_to_AP(cJSON* data, sqlite3* db)
{
    M1_LOG_DEBUG("M1_write_to_AP\n");
    int sn = 2;
    int row_n;
    int clientFd;
    int rc,ret = M1_PROTOCOL_OK;
    const char* ap_id = NULL;
    char* sql = (char*)malloc(300);
    sqlite3_stmt* stmt = NULL,*stmt_1 = NULL;
    cJSON* snJson = NULL;
    cJSON* pduJson = NULL;
    cJSON* devDataJson = NULL;
    cJSON* dataArrayJson = NULL;
    cJSON* devIdJson = NULL;
    
    if(data == NULL){
        M1_LOG_ERROR("data NULL");
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }

    /*更改sn*/
    snJson = cJSON_GetObjectItem(data, "sn");
    if(snJson == NULL){
        M1_LOG_ERROR("snJson NULL");
        ret = M1_PROTOCOL_FAILED;
        goto Finish;    
    }
    cJSON_SetIntValue(snJson, sn);
    /*获取clientFd*/
    pduJson = cJSON_GetObjectItem(data, "pdu");
    devDataJson = cJSON_GetObjectItem(pduJson, "devData");
    dataArrayJson = cJSON_GetArrayItem(devDataJson, 0);
    devIdJson = cJSON_GetObjectItem(dataArrayJson, "devId");
    M1_LOG_DEBUG("devId:%s\n",devIdJson->valuestring);
    /*get apId*/
    sprintf(sql,"select AP_ID from all_dev where DEV_ID = \"%s\" order by ID desc limit 1;",devIdJson->valuestring);
    if(sqlite3_prepare_v2(db, sql, strlen(sql),&stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        ret = M1_PROTOCOL_FAILED;
        goto Finish; 
    }
    rc = thread_sqlite3_step(&stmt,db);
    if(rc == SQLITE_ROW){
        ap_id = sqlite3_column_text(stmt,0);
        if(ap_id == NULL){
            M1_LOG_ERROR( "ap_id NULL\n");  
            ret = M1_PROTOCOL_FAILED;
            goto Finish;
        }
        M1_LOG_DEBUG("ap_id%s\n",ap_id);
    }else{
        ret = M1_PROTOCOL_FAILED;
        goto Finish; 
    }

    /*get clientFd*/
    sprintf(sql,"select CLIENT_FD from conn_info where AP_ID = \"%s\" order by ID desc limit 1;",ap_id);
    if(sqlite3_prepare_v2(db, sql, strlen(sql),&stmt_1, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        ret = M1_PROTOCOL_FAILED;
        goto Finish; 
    }
    rc = thread_sqlite3_step(&stmt_1, db);
    if(rc == SQLITE_ROW){
        clientFd = sqlite3_column_int(stmt_1,0);

        char * p = cJSON_PrintUnformatted(data);
        
        if(NULL == p)
        {    
            cJSON_Delete(data);
            ret = M1_PROTOCOL_FAILED;
            goto Finish;  
        }

        M1_LOG_DEBUG("string:%s\n",p);
        /*response to client*/
        socketSeverSend((uint8*)p, strlen(p), clientFd);
    }

    Finish:
    free(sql);
    sqlite3_finalize(stmt);
    sqlite3_finalize(stmt_1);

    return ret;
}

static int APP_write_handle(payload_t data)
{
    int i,j;
    int number1,number2;
    int row_n,id;
    int rc, ret = M1_PROTOCOL_OK;
    char* errorMsg = NULL;
    char* time = (char*)malloc(30);
    char* sql = NULL;
    char* sql_1 = (char*)malloc(300);
    char* sql_2 = (char*)malloc(300);
#if (!SQL_HISTORY_DEL)
    char* sql_3 = (char*)malloc(300);
    sqlite3_stmt* stmt_3 = NULL;
#endif
    const char* dev_name = NULL;
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL,*stmt_1 = NULL;
    cJSON* devDataJson = NULL;
    cJSON* devIdJson = NULL;
    cJSON* paramDataJson = NULL;
    cJSON* paramArrayJson = NULL;
    cJSON* valueTypeJson = NULL;
    cJSON* valueJson = NULL;

    M1_LOG_DEBUG("APP_write_handle\n");
    if(data.pdu == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    };

    getNowTime(time);
    /*获取数据库*/
    db = data.db;
    /*关闭写同步*/
    if(sqlite3_exec(db,"PRAGMA synchronous = OFF;",NULL,NULL,&errorMsg) != SQLITE_OK){
        M1_LOG_ERROR("PRAGMA synchronous = OFF falied:%s\n",errorMsg);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;  
    }

    sql = "select ID from param_table order by ID desc limit 1";
    id = sql_id(db, sql);
    M1_LOG_DEBUG("id:%d\n",id);
    if(sqlite3_exec(db, "BEGIN", NULL, NULL, &errorMsg)==SQLITE_OK){
        M1_LOG_DEBUG("BEGIN\n");
        /*insert data*/
        sprintf(sql_2,"insert into param_table(ID, DEV_NAME,DEV_ID,TYPE,VALUE,TIME) values(?,?,?,?,?,?);");
        number1 = cJSON_GetArraySize(data.pdu);
        M1_LOG_DEBUG("number1:%d\n",number1);
        for(i = 0; i < number1; i++){
            devDataJson = cJSON_GetArrayItem(data.pdu, i);
            if(devDataJson == NULL)
            {
                ret = M1_PROTOCOL_FAILED;
                goto Finish;
            }
            devIdJson = cJSON_GetObjectItem(devDataJson, "devId");
            if(devIdJson == NULL)
            {
                ret = M1_PROTOCOL_FAILED;
                goto Finish;
            }
            M1_LOG_DEBUG("devId:%s\n",devIdJson->valuestring);
            paramDataJson = cJSON_GetObjectItem(devDataJson, "param");
            if(paramDataJson == NULL)
            {
                ret = M1_PROTOCOL_FAILED;
                goto Finish;
            }
            number2 = cJSON_GetArraySize(paramDataJson);
            M1_LOG_DEBUG("number2:%d\n",number2);
            sprintf(sql_1,"select DEV_NAME from all_dev where DEV_ID = \"%s\" order by ID desc limit 1;", devIdJson->valuestring);
            M1_LOG_DEBUG("sql_1:%s\n",sql_1);

            sqlite3_finalize(stmt);
            if(sqlite3_prepare_v2(db, sql_1, strlen(sql_1), &stmt, NULL) != SQLITE_OK){
                M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                ret = M1_PROTOCOL_FAILED;
                goto Finish; 
            }
            rc = thread_sqlite3_step(&stmt, db);
            if(rc == SQLITE_ROW){
                dev_name = (const char*)sqlite3_column_text(stmt, 0);
                if(dev_name == NULL)
                {
                    ret = M1_PROTOCOL_FAILED;
                    goto Finish;
                }           
                M1_LOG_DEBUG("dev_name:%s\n",dev_name);
            }else{
                M1_LOG_WARN("dev_name not exit\n");
                continue;
            }
            for(j = 0; j < number2; j++){
                paramArrayJson = cJSON_GetArrayItem(paramDataJson, j);
                if(paramArrayJson == NULL)
                {
                    ret = M1_PROTOCOL_FAILED;
                    goto Finish;
                }
                valueTypeJson = cJSON_GetObjectItem(paramArrayJson, "type");
                if(valueTypeJson == NULL)
                {
                    ret = M1_PROTOCOL_FAILED;
                    goto Finish;
                }
                M1_LOG_DEBUG("  type%d:%d\n",j,valueTypeJson->valueint);
                valueJson = cJSON_GetObjectItem(paramArrayJson, "value");
                if(valueJson == NULL)
                {
                    ret = M1_PROTOCOL_FAILED;
                    goto Finish;
                }
                M1_LOG_DEBUG("  value%d:%d\n",j,valueJson->valueint);
#if (!SQL_HISTORY_DEL)
                M1_LOG_WARN("APP update\n");
                sprintf(sql_3,"update param_table set VALUE = %05d where DEV_ID = \"%s\" and TYPE = %05d;",valueJson->valueint,devIdJson->valuestring,valueTypeJson->valueint);
                M1_LOG_DEBUG("sql_1:%s\n",sql_3);
                sqlite3_finalize(stmt_3);
                if(sqlite3_prepare_v2(db, sql_3, strlen(sql_3), &stmt_3, NULL) != SQLITE_OK){
                    M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                    ret = M1_PROTOCOL_FAILED;
                    goto Finish; 
                }   
                rc = thread_sqlite3_step(&stmt_3, db);
                if(rc != SQLITE_ROW){
                    sqlite3_finalize(stmt_1); 
                    if(sqlite3_prepare_v2(db, sql_2, strlen(sql_2), &stmt_1, NULL) != SQLITE_OK){
                        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                        ret = M1_PROTOCOL_FAILED;
                        goto Finish; 
                    }
                    sqlite3_bind_int(stmt_1, 1, id);
                    id++;
                    sqlite3_bind_text(stmt_1, 2,  dev_name, -1, NULL);
                    sqlite3_bind_text(stmt_1, 3, devIdJson->valuestring, -1, NULL);
                    sqlite3_bind_int(stmt_1, 4, valueTypeJson->valueint);
                    sqlite3_bind_int(stmt_1, 5, valueJson->valueint);
                    sqlite3_bind_text(stmt_1, 6,  time, -1, NULL);
                    rc = thread_sqlite3_step(&stmt_1, db);
                }
#else
                M1_LOG_WARN("APP insert\n");
                sqlite3_finalize(stmt_1); 
                if(sqlite3_prepare_v2(db, sql_2, strlen(sql_2), &stmt_1, NULL) != SQLITE_OK){
                    M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                    ret = M1_PROTOCOL_FAILED;
                    goto Finish; 
                }
                sqlite3_bind_int(stmt_1, 1, id);
                id++;
                sqlite3_bind_text(stmt_1, 2,  dev_name, -1, NULL);
                sqlite3_bind_text(stmt_1, 3, devIdJson->valuestring, -1, NULL);
                sqlite3_bind_int(stmt_1, 4, valueTypeJson->valueint);
                sqlite3_bind_int(stmt_1, 5, valueJson->valueint);
                sqlite3_bind_text(stmt_1, 6,  time, -1, NULL);
                rc = thread_sqlite3_step(&stmt_1, db);
#endif
            }
        }
        if(sqlite3_exec(db, "COMMIT", NULL, NULL, &errorMsg) == SQLITE_OK){
            M1_LOG_DEBUG("END\n");
        }else{
            M1_LOG_DEBUG("ROLLBACK\n");
            if(sqlite3_exec(db, "ROLLBACK", NULL, NULL, &errorMsg) == SQLITE_OK){
                M1_LOG_DEBUG("ROLLBACK OK\n");
                sqlite3_free(errorMsg);
            }else{
                M1_LOG_ERROR("ROLLBACK FALIED\n");
            }
        }
    }else{
        M1_LOG_ERROR("errorMsg:");
    }    

    Finish:
    free(time);
    free(sql_1);
    free(sql_2);
#if (!SQL_HISTORY_DEL)
    free(sql_3);
    sqlite3_finalize(stmt_3);
#endif
    sqlite3_free(errorMsg);
    sqlite3_finalize(stmt);
    sqlite3_finalize(stmt_1);

    return ret;
}

static int APP_echo_dev_info_handle(payload_t data)
{
    int i,j;
    int number,number_1;
    int rc,ret = M1_PROTOCOL_OK;
    char* err_msg = NULL;
    char* sql = NULL;
    char* errorMsg = NULL;
    /*sqlite3*/
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;
    cJSON* devDataJson = NULL;
    cJSON* devdataArrayJson = NULL;
    cJSON* devArrayJson = NULL;
    cJSON* APIdJson = NULL;

    M1_LOG_DEBUG("APP_echo_dev_info_handle\n");
    if(data.pdu == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    } 

    db = data.db;
    sql = (char*)malloc(300);
    number = cJSON_GetArraySize(data.pdu);
    M1_LOG_DEBUG("number:%d\n",number);  
    for(i = 0; i < number; i++){
        devdataArrayJson = cJSON_GetArrayItem(data.pdu, i);
        if(devdataArrayJson == NULL){
            M1_LOG_ERROR("devdataArrayJson NULL\n");
            ret = M1_PROTOCOL_FAILED;
            goto Finish;
        }
        APIdJson = cJSON_GetObjectItem(devdataArrayJson, "apId");
        if(APIdJson == NULL){
            M1_LOG_ERROR("APIdJson NULL\n");
            ret = M1_PROTOCOL_FAILED;
            goto Finish;
        }
        devDataJson = cJSON_GetObjectItem(devdataArrayJson,"devId");

        M1_LOG_DEBUG("AP_ID:%s\n",APIdJson->valuestring);            
        sprintf(sql, "update all_dev set ADDED = 1, STATUS = \"ON\" where DEV_ID = \"%s\" and AP_ID = \"%s\";",APIdJson->valuestring,APIdJson->valuestring);
        M1_LOG_DEBUG("sql:%s\n",sql);
        rc = sqlite3_exec(db, sql, NULL, NULL, &errorMsg);
            if(rc != SQLITE_OK){
            M1_LOG_ERROR("update all_dev fail: %s\n",errorMsg);
        }

        if(devDataJson != NULL){
            number_1 = cJSON_GetArraySize(devDataJson);
            M1_LOG_DEBUG("number_1:%d\n",number_1);
            for(j = 0; j < number_1; j++){
                devArrayJson = cJSON_GetArrayItem(devDataJson, j);
                M1_LOG_DEBUG("  devId:%s\n",devArrayJson->valuestring);

                sprintf(sql, "update all_dev set ADDED = 1, STATUS = \"ON\" where DEV_ID = \"%s\" and AP_ID = \"%s\";",devArrayJson->valuestring,APIdJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sqlite3_finalize(stmt);
                if(sqlite3_prepare_v2(db, sql, strlen(sql),&stmt, NULL) != SQLITE_OK){
                    M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                    ret = M1_PROTOCOL_FAILED;
                    goto Finish; 
                }
                thread_sqlite3_step(&stmt, db);
            }
        }
    }  

    Finish:
    free(sql);
    sqlite3_finalize(stmt);

    return ret;
}

static int APP_req_added_dev_info_handle(payload_t data)
{
    /*cJSON*/
    int row_n;
    int rc,ret = M1_PROTOCOL_OK;
    int pduType = TYPE_M1_REPORT_ADDED_INFO;
    char* account = NULL;
    char* sql = (char*)malloc(300);
    char* sql_1 = (char*)malloc(300);;
    char* sql_2 = (char*)malloc(300);;
    cJSON*  devDataObject= NULL;
    cJSON* devArray = NULL;
    cJSON*  devObject = NULL;
    cJSON* pJsonRoot = NULL;
    cJSON * devDataJsonArray = NULL;
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL, *stmt_1 = NULL,*stmt_2 = NULL;

    M1_LOG_DEBUG("APP_req_added_dev_info_handle\n");
    db = data.db;
    pJsonRoot = cJSON_CreateObject();
    if(NULL == pJsonRoot)
    {
        M1_LOG_ERROR("pJsonRoot NULL\n");
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }

    cJSON_AddNumberToObject(pJsonRoot, "sn", data.sn);
    cJSON_AddStringToObject(pJsonRoot, "version", "1.0");
    cJSON_AddNumberToObject(pJsonRoot, "netFlag", 1);
    cJSON_AddNumberToObject(pJsonRoot, "cmdType", 1);
    /*create pdu object*/
    cJSON * pduJsonObject = NULL;
    pduJsonObject = cJSON_CreateObject();
    if(NULL == pduJsonObject)
    {
        // create object faild, exit
        cJSON_Delete(pduJsonObject);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    /*add pdu to root*/
    cJSON_AddItemToObject(pJsonRoot, "pdu", pduJsonObject);
    /*add pdu type to pdu object*/
    cJSON_AddNumberToObject(pduJsonObject, "pduType", pduType);
    /*create devData array*/
    devDataJsonArray = cJSON_CreateArray();
    if(NULL == devDataJsonArray)
    {
        cJSON_Delete(devDataJsonArray);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    /*add devData array to pdu pbject*/
    cJSON_AddItemToObject(pduJsonObject, "devData", devDataJsonArray); 
    /*获取当前账户*/
    sprintf(sql,"select ACCOUNT from account_info where CLIENT_FD = %03d order by ID desc limit 1;",data.clientFd);
    M1_LOG_DEBUG( "%s\n", sql);
    sqlite3_finalize(stmt);
    if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        ret = M1_PROTOCOL_FAILED;
        goto Finish; 
    }
    if(thread_sqlite3_step(&stmt, db) == SQLITE_ROW){
        account =  sqlite3_column_text(stmt, 0);
        if(account == NULL){
            M1_LOG_ERROR( "user account do not exist\n");    
            ret = M1_PROTOCOL_FAILED;
            goto Finish;
        }else{
            M1_LOG_DEBUG("account:%s\n",account);
        }
    }
    
    sprintf(sql_1,"select * from all_dev where DEV_ID  = AP_ID and ACCOUNT = \"%s\";",account);
    if(sqlite3_prepare_v2(db, sql_1, strlen(sql_1),&stmt_1, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        ret = M1_PROTOCOL_FAILED;
        goto Finish; 
    }
    while(thread_sqlite3_step(&stmt_1, db) == SQLITE_ROW){
        /*add ap infomation: port,ap_id,ap_name,time */
        devDataObject = cJSON_CreateObject();
        if(NULL == devDataObject)
        {
            // create object faild, exit
            M1_LOG_ERROR("devDataObject NULL\n");
            cJSON_Delete(devDataObject);
            ret = M1_PROTOCOL_FAILED;
            goto Finish;
        }
        cJSON_AddItemToArray(devDataJsonArray, devDataObject);
        cJSON_AddNumberToObject(devDataObject, "pId", sqlite3_column_int(stmt_1, 4));
        cJSON_AddStringToObject(devDataObject, "apId",  (const char*)sqlite3_column_text(stmt_1, 3));
        cJSON_AddStringToObject(devDataObject, "apName", (const char*)sqlite3_column_text(stmt_1, 2));
            
        /*create devData array*/
        devArray = cJSON_CreateArray();
        if(NULL == devArray)
        {
            M1_LOG_ERROR("devArry NULL\n");
            cJSON_Delete(devArray);
            ret = M1_PROTOCOL_FAILED;
            goto Finish;
        }
        /*add devData array to pdu pbject*/
        cJSON_AddItemToObject(devDataObject, "dev", devArray);
        /*sqlite3*/
        sprintf(sql_2,"select * from all_dev where AP_ID  = \"%s\" and AP_ID != DEV_ID and ACCOUNT = \"%s\";",sqlite3_column_text(stmt_1, 3),account);
        M1_LOG_DEBUG("sql_2:%s\n",sql_2);

        sqlite3_finalize(stmt_2);
        if(sqlite3_prepare_v2(db, sql_2, strlen(sql_2),&stmt_2, NULL) != SQLITE_OK){
            M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
            ret = M1_PROTOCOL_FAILED;
            goto Finish; 
        }
        while(thread_sqlite3_step(&stmt_2, db) == SQLITE_ROW){
             /*add ap infomation: port,ap_id,ap_name,time */
            devObject = cJSON_CreateObject();
            if(NULL == devObject)
            {
                M1_LOG_ERROR("devObject NULL\n");
                cJSON_Delete(devObject);
                ret = M1_PROTOCOL_FAILED;
                goto Finish;
            }
            cJSON_AddItemToArray(devArray, devObject); 
            cJSON_AddNumberToObject(devObject, "pId", sqlite3_column_int(stmt_2, 4));
            cJSON_AddStringToObject(devObject, "devId", (const char*)sqlite3_column_text(stmt_2, 1));
            cJSON_AddStringToObject(devObject, "devName", (const char*)sqlite3_column_text(stmt_2, 2));
        }
         

    }
   

    char * p = cJSON_PrintUnformatted(pJsonRoot);
    if(NULL == p)
    {    
        cJSON_Delete(pJsonRoot);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }

    M1_LOG_DEBUG("string:%s\n",p);
    /*response to client*/
    socketSeverSend((uint8*)p, strlen(p), data.clientFd);
    
    Finish:
    free(sql);
    free(sql_1);
    free(sql_2);
    sqlite3_finalize(stmt);
    sqlite3_finalize(stmt_1);
    sqlite3_finalize(stmt_2);
    cJSON_Delete(pJsonRoot);

    return ret;
}

static int APP_net_control(payload_t data)
{
    M1_LOG_DEBUG("APP_net_control\n");
    int clientFd, row_n; 
    int rc,ret = M1_PROTOCOL_OK; 
    int pduType = TYPE_DEV_NET_CONTROL;
    char* sql = (char*)malloc(300); 
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;
    cJSON * pJsonRoot = NULL;
    cJSON* apIdJson = NULL;
    cJSON* valueJson = NULL;
    cJSON * pduJsonObject = NULL;

    if(data.pdu == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    };

    db = data.db;
    apIdJson = cJSON_GetObjectItem(data.pdu, "apId");
    if(apIdJson == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    };
    M1_LOG_DEBUG("apId:%s\n",apIdJson->valuestring);
    valueJson = cJSON_GetObjectItem(data.pdu, "value");
    if(valueJson == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    };
    M1_LOG_DEBUG("value:%d\n",valueJson->valueint);  

    sprintf(sql,"select CLIENT_FD from conn_info where AP_ID = \"%s\";",apIdJson->valuestring);
    M1_LOG_DEBUG("sql:%s\n",sql);
    if(sqlite3_prepare_v2(db, sql, strlen(sql),&stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        ret = M1_PROTOCOL_FAILED;
        goto Finish; 
    }
    rc = thread_sqlite3_step(&stmt, db);
    M1_LOG_DEBUG("step() return %s\n", rc == SQLITE_DONE ? "SQLITE_DONE": rc == SQLITE_ROW ? "SQLITE_ROW" : "SQLITE_ERROR");
    if(rc == SQLITE_ROW){
        clientFd = sqlite3_column_int(stmt,0);
        M1_LOG_DEBUG("clientFd:%05d\n",clientFd);          
    }else{
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    /*create json*/
    pJsonRoot = cJSON_CreateObject();
    if(NULL == pJsonRoot)
    {
        M1_LOG_ERROR("pJsonRoot NULL\n");
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }

    cJSON_AddNumberToObject(pJsonRoot, "sn", 1);
    cJSON_AddStringToObject(pJsonRoot, "version", "1.0");
    cJSON_AddNumberToObject(pJsonRoot, "netFlag", 1);
    cJSON_AddNumberToObject(pJsonRoot, "cmdType", 2);
    /*create pdu object*/
    pduJsonObject = cJSON_CreateObject();
    if(NULL == pduJsonObject)
    {
        // create object faild, exit
        cJSON_Delete(pduJsonObject);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    /*add pdu to root*/
    cJSON_AddItemToObject(pJsonRoot, "pdu", pduJsonObject);
    /*add pdu type to pdu object*/
    cJSON_AddNumberToObject(pduJsonObject, "pduType", pduType);
    /*add dev data to pdu object*/
    cJSON_AddNumberToObject(pduJsonObject, "devData", valueJson->valueint);


    char * p = cJSON_PrintUnformatted(pJsonRoot);
    
    if(NULL == p)
    {    
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }

    M1_LOG_DEBUG("string:%s\n",p);
    /*response to client*/
    socketSeverSend((uint8*)p, strlen(p), clientFd);
    
    Finish:
    free(sql);
    sqlite3_finalize(stmt);
    cJSON_Delete(pJsonRoot);

    return ret;
}

static int M1_report_ap_info(payload_t data)
{
    M1_LOG_DEBUG(" M1_report_ap_info\n");

    int row_n;
    int rc, ret = M1_PROTOCOL_OK;
    int pduType = TYPE_M1_REPORT_AP_INFO;
    char* account = NULL;
    char* sql = (char*)malloc(300);
    cJSON*  devDataObject= NULL;
    cJSON * pJsonRoot = NULL;
    cJSON * pduJsonObject = NULL;
    cJSON * devDataJsonArray = NULL;
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;

    pJsonRoot = cJSON_CreateObject();
    if(NULL == pJsonRoot)
    {
        M1_LOG_ERROR("pJsonRoot NULL\n");
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }

    db = data.db;
    cJSON_AddNumberToObject(pJsonRoot, "sn", data.sn);
    cJSON_AddStringToObject(pJsonRoot, "version", "1.0");
    cJSON_AddNumberToObject(pJsonRoot, "netFlag", 1);
    cJSON_AddNumberToObject(pJsonRoot, "cmdType", 1);
    /*create pdu object*/
    pduJsonObject = cJSON_CreateObject();
    if(NULL == pduJsonObject)
    {
        // create object faild, exit
        cJSON_Delete(pduJsonObject);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    /*add pdu to root*/
    cJSON_AddItemToObject(pJsonRoot, "pdu", pduJsonObject);
    /*add pdu type to pdu object*/
    cJSON_AddNumberToObject(pduJsonObject, "pduType", pduType);
    /*create devData array*/
    devDataJsonArray = cJSON_CreateArray();
    if(NULL == devDataJsonArray)
    {
        cJSON_Delete(devDataJsonArray);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    /*add devData array to pdu pbject*/
    cJSON_AddItemToObject(pduJsonObject, "devData", devDataJsonArray);
    /*获取用户账户信息*/
    sprintf(sql,"select ACCOUNT from account_info where CLIENT_FD = %03d order by ID desc limit 1;",data.clientFd);
    M1_LOG_DEBUG( "%s\n", sql);
    sqlite3_finalize(stmt);
    if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        ret = M1_PROTOCOL_FAILED;
        goto Finish; 
    }
    if(thread_sqlite3_step(&stmt, db) == SQLITE_ROW){
        account =  sqlite3_column_text(stmt, 0);
        if(account == NULL){
            ret = M1_PROTOCOL_FAILED;
            goto Finish;
        }else{
            M1_LOG_DEBUG("clientFd:%03d,account:%s\n",data.clientFd, account);
        }
    }

    sprintf(sql,"select * from all_dev where DEV_ID = AP_ID and ACCOUNT = \"%s\";",account);
    row_n = sql_row_number(db, sql);

    sqlite3_finalize(stmt);
    if(sqlite3_prepare_v2(db, sql, strlen(sql),&stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        ret = M1_PROTOCOL_FAILED;
        goto Finish; 
    }
    while(thread_sqlite3_step(&stmt, db) == SQLITE_ROW){
        /*add ap infomation: port,ap_id,ap_name,time */
        devDataObject = cJSON_CreateObject();
        if(NULL == devDataObject)
        {
            // create object faild, exit
            M1_LOG_ERROR("devDataObject NULL\n");
            cJSON_Delete(devDataObject);
            ret = M1_PROTOCOL_FAILED;
            goto Finish;
        }
        cJSON_AddItemToArray(devDataJsonArray, devDataObject);
        cJSON_AddNumberToObject(devDataObject, "pId", sqlite3_column_int(stmt, 4));
        cJSON_AddStringToObject(devDataObject, "apId", (const char*)sqlite3_column_text(stmt, 3));
        cJSON_AddStringToObject(devDataObject, "apName", (const char*)sqlite3_column_text(stmt, 2));
        
    }

    char * p = cJSON_PrintUnformatted(pJsonRoot);
    
    if(NULL == p)
    {    
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }

    M1_LOG_DEBUG("string:%s\n",p);
    /*response to client*/
    socketSeverSend((uint8*)p, strlen(p), data.clientFd);
    Finish:
    free(sql);
    sqlite3_finalize(stmt);
    cJSON_Delete(pJsonRoot);

    return ret;
}

static int M1_report_dev_info(payload_t data)
{
    M1_LOG_DEBUG(" M1_report_dev_info\n");
    int row_n;
    int pduType = TYPE_M1_REPORT_DEV_INFO;
    int rc, ret = M1_PROTOCOL_OK;
    char* ap = NULL;
    char* account = NULL;
    char* sql = (char*)malloc(300);
    cJSON * pJsonRoot = NULL;
    cJSON * pduJsonObject = NULL;
    cJSON * devDataJsonArray = NULL;
    cJSON*  devDataObject= NULL;
    /*sqlite3*/
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;

    db = data.db;
    ap = data.pdu->valuestring;
    pJsonRoot = cJSON_CreateObject();
    if(NULL == pJsonRoot)
    {
        M1_LOG_ERROR("pJsonRoot NULL\n");
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }

    cJSON_AddNumberToObject(pJsonRoot, "sn", data.sn);
    cJSON_AddStringToObject(pJsonRoot, "version", "1.0");
    cJSON_AddNumberToObject(pJsonRoot, "netFlag", 1);
    cJSON_AddNumberToObject(pJsonRoot, "cmdType", 1);
    /*create pdu object*/
    pduJsonObject = cJSON_CreateObject();
    if(NULL == pduJsonObject)
    {
        // create object faild, exit
        cJSON_Delete(pduJsonObject);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    /*add pdu to root*/
    cJSON_AddItemToObject(pJsonRoot, "pdu", pduJsonObject);
    /*add pdu type to pdu object*/
    cJSON_AddNumberToObject(pduJsonObject, "pduType", pduType);
    /*create devData array*/
    devDataJsonArray = cJSON_CreateArray();
    if(NULL == devDataJsonArray)
    {
        cJSON_Delete(devDataJsonArray);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    /*add devData array to pdu pbject*/
    cJSON_AddItemToObject(pduJsonObject, "devData", devDataJsonArray);
    /*获取用户账户信息*/
    sprintf(sql,"select ACCOUNT from account_info where CLIENT_FD = %03d order by ID desc limit 1;",data.clientFd);
    M1_LOG_DEBUG( "%s\n", sql);
    sqlite3_finalize(stmt);
    if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        ret = M1_PROTOCOL_FAILED;
        goto Finish; 
    }
    if(thread_sqlite3_step(&stmt, db) == SQLITE_ROW){
        account =  sqlite3_column_text(stmt, 0);
        if(account == NULL){
            ret = M1_PROTOCOL_FAILED;
            goto Finish;
        }else{
            M1_LOG_DEBUG("clientFd:%03d,account:%s\n",data.clientFd, account);
        }
    }

    sprintf(sql,"select * from all_dev where AP_ID != DEV_ID and AP_ID = \"%s\" and  ACCOUNT = \"%s\";", ap, account);
    M1_LOG_DEBUG("string:%s\n",sql);

    sqlite3_finalize(stmt);
    if(sqlite3_prepare_v2(db, sql, strlen(sql),&stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        ret = M1_PROTOCOL_FAILED;
        goto Finish; 
    }
    while(thread_sqlite3_step(&stmt,db) == SQLITE_ROW){
        /*add ap infomation: port,ap_id,ap_name,time */
        devDataObject = cJSON_CreateObject();
        if(NULL == devDataObject)
        {
            // create object faild, exit
            M1_LOG_ERROR("devDataObject NULL\n");
            cJSON_Delete(devDataObject);
            ret = M1_PROTOCOL_FAILED;
            goto Finish;
        }
        cJSON_AddItemToArray(devDataJsonArray, devDataObject);
        cJSON_AddNumberToObject(devDataObject, "pId", sqlite3_column_int(stmt, 4));
        cJSON_AddStringToObject(devDataObject, "devId", (const char*)sqlite3_column_text(stmt, 1));
        cJSON_AddStringToObject(devDataObject, "devName", (const char*)sqlite3_column_text(stmt, 2));
        
    }

    char * p = cJSON_PrintUnformatted(pJsonRoot);
    
    if(NULL == p)
    {    
        cJSON_Delete(pJsonRoot);
        return M1_PROTOCOL_FAILED;
    }

    M1_LOG_DEBUG("string:%s\n",p);
    /*response to client*/
    socketSeverSend((uint8*)p, strlen(p), data.clientFd);
    
    Finish:
    free(sql);
    sqlite3_finalize(stmt);
    cJSON_Delete(pJsonRoot);

    return ret;
}

/*子设备/AP/联动/场景/区域/-启动/停止/删除*/
static int common_operate(payload_t data)
{
    M1_LOG_DEBUG("common_operate\n");
    int pduType = TYPE_COMMON_OPERATE;
    int id;
    int rc,ret = M1_PROTOCOL_OK; 
    int number1,i;
    int row_number = 0;
    char*scen_name = NULL;
    char* time = (char*)malloc(30);
    char* sql = (char*)malloc(300);
    char* errorMsg = NULL;
    cJSON* typeJson = NULL;
    cJSON* idJson = NULL;
    cJSON* operateJson = NULL;    
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;

    if(data.pdu == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    
    getNowTime(time);

    typeJson = cJSON_GetObjectItem(data.pdu, "type");
    if(typeJson == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    M1_LOG_DEBUG("type:%s\n",typeJson->valuestring);
    idJson = cJSON_GetObjectItem(data.pdu, "id");   
    if(idJson == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    M1_LOG_DEBUG("id:%s\n",idJson->valuestring);
    operateJson = cJSON_GetObjectItem(data.pdu, "operate");   
    if(operateJson == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    M1_LOG_DEBUG("operate:%s\n",operateJson->valuestring);
    /*获取数据库*/
    db = data.db;
    if(sqlite3_exec(db, "BEGIN", NULL, NULL, &errorMsg)==SQLITE_OK){
        M1_LOG_DEBUG("BEGIN\n");
        if(strcmp(typeJson->valuestring, "device") == 0){
            if(strcmp(operateJson->valuestring, "delete") == 0){
                /*通知到ap*/
                if(m1_del_dev_from_ap(db, idJson->valuestring) != M1_PROTOCOL_OK)
                    M1_LOG_ERROR("m1_del_dev_from_ap error\n");
                /*删除all_dev中的子设备*/
                sprintf(sql,"delete from all_dev where DEV_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除scenario_table中的子设备*/
                sprintf(sql,"delete from scenario_table where DEV_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除link_trigger_table中的子设备*/
                sprintf(sql,"delete from link_trigger_table where DEV_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除link_exec_table中的子设备*/
                sprintf(sql,"delete from link_exec_table where DEV_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                #if 0
                /*删除联动表linkage_table中相关内容*/
                sprintf(sql,"delete from linkage_table where EXEC_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                #endif
            }else if(strcmp(operateJson->valuestring, "on") == 0){
                sprintf(sql,"update all_dev set STATUS = \"ON\" where DEV_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*更新启停状态*/
                update_param_tb_t dev_status;
                dev_status.devId = idJson->valuestring;
                dev_status.type = 0x2022;
                dev_status.value = 1;
                app_update_param_table(dev_status, db);
            }else if(strcmp(operateJson->valuestring, "off") == 0){
                sprintf(sql,"update all_dev set STATUS = \"OFF\" where DEV_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*更新启停状态*/
                update_param_tb_t dev_status;
                dev_status.devId = idJson->valuestring;
                dev_status.type = 0x2022;
                dev_status.value = 0;
                app_update_param_table(dev_status, db);
            }

        }else if(strcmp(typeJson->valuestring, "linkage") == 0){
            if(strcmp(operateJson->valuestring, "delete") == 0){
                /*删除联动表linkage_table中相关内容*/
                sprintf(sql,"delete from linkage_table where LINK_NAME = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
      
                /*删除联动触发表link_trigger_table相关内容*/
                sprintf(sql,"delete from link_trigger_table where LINK_NAME = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除联动触发表link_exec_table相关内容*/
                sprintf(sql,"delete from link_exec_table where LINK_NAME = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
            }
        }else if(strcmp(typeJson->valuestring, "scenario") == 0){
            if(strcmp(operateJson->valuestring, "delete") == 0){
                /*删除场景表相关内容*/
                sprintf(sql,"delete from scenario_table where SCEN_NAME = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除场景定时相关内容*/
                sprintf(sql,"delete from scen_alarm_table where SCEN_NAME = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除联动表linkage_table中相关内容*/
                sprintf(sql,"delete from linkage_table where EXEC_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
            }
        }else if(strcmp(typeJson->valuestring, "district") == 0){
            if(strcmp(operateJson->valuestring, "delete") == 0){
                /*删除区域相关内容*/
                sprintf(sql,"delete from district_table where DIS_NAME = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除区域下的联动表中相关内容*/
                sprintf(sql,"delete from linkage_table where DISTRICT = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);    
                /*删除区域下的联动触发表中相关内容*/
                sprintf(sql,"delete from link_trigger_table where DISTRICT = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);    
                /*删除区域下的联动触发表中相关内容*/
                sprintf(sql,"delete from link_exec_table where DISTRICT = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql); 
                /*删除场景定时相关内容*/
                sprintf(sql,"select SCEN_NAME from scenario_table where DISTRICT = \"%s\" order by ID desc limit 1;",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sqlite3_finalize(stmt);
                if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
                    M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                    ret = M1_PROTOCOL_FAILED;
                    goto Finish; 
                }
                rc = thread_sqlite3_step(&stmt, db);
                M1_LOG_DEBUG("step() return %s\n", rc == SQLITE_DONE ? "SQLITE_DONE": rc == SQLITE_ROW ? "SQLITE_ROW" : "SQLITE_ERROR");
                if(rc == SQLITE_ROW){
                    scen_name = sqlite3_column_text(stmt,0);
                    if(scen_name == NULL){
                        ret = M1_PROTOCOL_FAILED;
                        goto Finish;
                    }
                }
                sprintf(sql,"delete from scen_alarm_table where SCEN_NAME = \"%s\";",scen_name);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql); 
                /*删除区域下场景表相关内容*/   
                sprintf(sql,"delete from scenario_table where DISTRICT = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql); 
            }
        }else if(strcmp(typeJson->valuestring, "account") == 0){
            if(strcmp(idJson->valuestring,"Dalitek") == 0){
                goto Finish;
            }
            if(strcmp(operateJson->valuestring, "delete") == 0){
                /*删除account_table中的信息*/
                sprintf(sql,"delete from account_table where ACCOUNT = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除all_dev中的信息*/
                sprintf(sql,"delete from all_dev where ACCOUNT = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除district_table中的信息*/
                sprintf(sql,"delete from district_table where ACCOUNT = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除scenario_table中的信息*/
                sprintf(sql,"delete from scenario_table where ACCOUNT = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
            }
        }else if(strcmp(typeJson->valuestring, "ap") == 0){
            /*删除all_dev中设备*/
            if(strcmp(operateJson->valuestring, "delete") == 0){
                /*通知ap*/
                if(m1_del_ap(db, idJson->valuestring) != M1_PROTOCOL_OK)
                    M1_LOG_ERROR("m1_del_ap error\n");
                /*删除AP下子设备相关连的联动业务*/
                #if 0
                clear_ap_related_linkage(idJson->valuestring, db);
                #endif
                ///*删除all_dev中的子设备*/
                sprintf(sql,"delete from all_dev where AP_ID = \"%s\";",idJson->valuestring);
                //sprintf(sql,"update all_dev set ADDED = 0,NET = 0,STATUS = \"OFF\" where AP_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除scenario_table中的子设备*/
                sprintf(sql,"delete from scenario_table where AP_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除link_trigger_table中的子设备*/
                sprintf(sql,"delete from link_trigger_table where AP_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除link_exec_table中的子设备*/
                sprintf(sql,"delete from link_exec_table where AP_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*删除district_table中的子设备*/
                sprintf(sql,"delete from district_table where AP_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
            }else if(strcmp(operateJson->valuestring, "on") == 0){
                sprintf(sql,"update all_dev set STATUS = \"ON\" where AP_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*更新启停状态*/
                update_param_tb_t dev_status;
                dev_status.devId = idJson->valuestring;
                dev_status.type = 0x2022;
                dev_status.value = 1;
                app_update_param_table(dev_status, db);
            }else if(strcmp(operateJson->valuestring, "off") == 0){
                sprintf(sql,"update all_dev set STATUS = \"OFF\" where AP_ID = \"%s\";",idJson->valuestring);
                M1_LOG_DEBUG("sql:%s\n",sql);
                sql_exec(db, sql);
                /*更新启停状态*/
                update_param_tb_t dev_status;
                dev_status.devId = idJson->valuestring;
                dev_status.type = 0x2022;
                dev_status.value = 0;
                app_update_param_table(dev_status, db);
            }
        }
        if(sqlite3_exec(db, "COMMIT", NULL, NULL, &errorMsg) == SQLITE_OK){
            M1_LOG_DEBUG("END\n");
        }else{
            M1_LOG_DEBUG("ROLLBACK\n");
            if(sqlite3_exec(db, "ROLLBACK", NULL, NULL, &errorMsg) == SQLITE_OK){
                M1_LOG_DEBUG("ROLLBACK OK\n");
                sqlite3_free(errorMsg);
            }else{
                M1_LOG_ERROR("ROLLBACK FALIED\n");
            }
        }
    }else{
        M1_LOG_ERROR("errorMsg:");
    }

    Finish:    
    free(time);
    free(sql);
    sqlite3_free(errorMsg);
    sqlite3_finalize(stmt);

    return ret;
}

#define AP_HEART_BEAT_INTERVAL   2   //min
/*查询离线设备*/
static void check_offline_dev(sqlite3*db)
{
    M1_LOG_DEBUG("check_offline_dev\n");
    int rc = 0;
    static char preTime[4] = {0};
    char* curTime = (char*)malloc(30);
    char *time = NULL;
    int u8CurTime = 0, u8Time = 0;
    char* apId = NULL;
    char* errorMsg = NULL;
    char* sql = NULL;
    char* sql_1 = (char*)malloc(300);
    char* sql_2 = (char*)malloc(300);
    sqlite3_stmt* stmt = NULL;
    sqlite3_stmt* stmt_1 = NULL;
    sqlite3_stmt* stmt_2 = NULL;

    getNowTime(curTime);
    /*当前时间*/
    M1_LOG_DEBUG("curTime:%x,%x,%x,%x\n",curTime[8],curTime[9],curTime[10],curTime[11]);
    u8CurTime = (curTime[8] - preTime[0])*60*10 + (curTime[9] - preTime[1]) * 60  + (curTime[10] - preTime[2]) * 10 + curTime[11] - preTime[3];
    u8CurTime = abs(u8CurTime);
    M1_LOG_DEBUG("u8CurTime:%d\n",u8CurTime);
    
    if(u8CurTime < 2){
        goto Finish;
    }
    memcpy(preTime, &curTime[8], 4);
    M1_LOG_DEBUG("preTime:%x,%x,%x,%x\n",preTime[0],preTime[1],preTime[2],preTime[3]);
    /*获取AP ID*/
    sql = "select AP_ID from all_dev where DEV_ID = AP_ID;";
    M1_LOG_DEBUG("string:%s\n",sql);
    if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        goto Finish; 
    }
    if(sqlite3_exec(db, "BEGIN", NULL, NULL, &errorMsg)==SQLITE_OK){
        M1_LOG_DEBUG("BEGIN:\n");
        while(thread_sqlite3_step(&stmt, db) == SQLITE_ROW){
            apId = sqlite3_column_text(stmt, 0);
            /*检查时间*/
            sprintf(sql_1,"select TIME from param_table where DEV_ID = \"%s\" and TYPE = 16404 order by ID desc limit 1;", apId);
            M1_LOG_DEBUG("string:%s\n",sql_1);
            sqlite3_finalize(stmt_1);
            if(sqlite3_prepare_v2(db, sql_1, strlen(sql_1), &stmt_1, NULL) != SQLITE_OK){
                M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                goto Finish; 
            }
            rc = thread_sqlite3_step(&stmt_1, db);
            if(rc != SQLITE_ROW){
                M1_LOG_DEBUG("rc != SQLITE_ROW\n");
                continue;
            }
            time = sqlite3_column_text(stmt_1, 0);
            if(time == NULL){
                M1_LOG_DEBUG("time == NULL\n");
                continue;
            }
             M1_LOG_DEBUG("time: %s\n",time);
            /*获取的上一次时间*/
            u8Time = (curTime[8] - time[8]) * 60 * 10 + (curTime[9] - time[9]) * 60 + (curTime[10] - time[10]) * 10 + curTime[11] - time[11];
            u8Time = abs(u8Time);
            M1_LOG_DEBUG("u8Time:%d\n",u8Time);
            if(u8Time > AP_HEART_BEAT_INTERVAL){
                sprintf(sql_2,"update param_table set VALUE = 0 where DEV_ID = \"%s\" and TYPE = 16404;", apId);
                sqlite3_finalize(stmt_2);
                if(sqlite3_prepare_v2(db, sql_2, strlen(sql_2), &stmt_2, NULL) != SQLITE_OK){
                    M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
                    goto Finish; 
                }
                rc = thread_sqlite3_step(&stmt_2, db);
                if(rc == SQLITE_ERROR){
                    M1_LOG_WARN("update failed\n");
                    continue;
                }
            }
        }

        if(sqlite3_exec(db, "COMMIT", NULL, NULL, &errorMsg) == SQLITE_OK){
            M1_LOG_DEBUG("END\n");
        }else{
            M1_LOG_DEBUG("ROLLBACK\n");
            if(sqlite3_exec(db, "ROLLBACK", NULL, NULL, &errorMsg) == SQLITE_OK){
                M1_LOG_DEBUG("ROLLBACK OK\n");
                sqlite3_free(errorMsg);
            }else{
                M1_LOG_ERROR("ROLLBACK FALIED\n");
            }
        }
    }else{
        M1_LOG_ERROR("errorMsg\n");
    }

    Finish:

    free(curTime);
    free(sql_1);
    free(sql_2);
    sqlite3_finalize(stmt);
    sqlite3_finalize(stmt_1);
    sqlite3_finalize(stmt_2);
}

static int ap_heartbeat_handle(payload_t data)
{
    M1_LOG_DEBUG("ap_heartbeat_handle\n");
#if AP_HEARTBEAT_HANDLE
    int rc = 0;
    int id = 0;
    int netValue = 0;
    char* devName = NULL;
    char* time = (char*)malloc(30);
    char* sql = (char*)malloc(300);
    char* sql_1 = NULL;
    int valueType = 0x4014;
    cJSON* apIdJson = NULL;
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL,*stmt_1 = NULL;

    if(data.pdu == NULL){
        M1_LOG_ERROR("data.pdu\n");
        goto Finish;
    }

    getNowTime(time);
    M1_LOG_DEBUG("now time:%s\n",time);
    db = data.db;
    apIdJson = data.pdu;
    M1_LOG_DEBUG("apIdJson:%s\n",apIdJson->valuestring);
    sprintf(sql,"select VALUE from param_table where DEV_ID = \"%s\" and TYPE = %05d;", apIdJson->valuestring,valueType);
    M1_LOG_DEBUG("string:%s\n",sql);
    if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        goto Finish; 
    }
    rc = thread_sqlite3_step(&stmt, db);
    if(rc == SQLITE_ROW){
        sprintf(sql,"update param_table set VALUE = 1 ,TIME = \"%s\" where DEV_ID = \"%s\" and TYPE = %05d;",time, apIdJson->valuestring,valueType);
        M1_LOG_DEBUG("string:%s\n",sql);
        sqlite3_finalize(stmt);
        if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
            M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
            goto Finish; 
        }
        rc = thread_sqlite3_step(&stmt, db);
        if(rc == SQLITE_ERROR){
            M1_LOG_WARN("update failed\n");
        }

    }

    Finish:
    free(sql);
    free(time);
    sqlite3_finalize(stmt);
    sqlite3_finalize(stmt_1);
#endif
    return M1_PROTOCOL_OK;
}

static int common_rsp(rsp_data_t data)
{
    M1_LOG_DEBUG(" common_rsp\n");
    
    int ret = M1_PROTOCOL_OK;
    int pduType = TYPE_COMMON_RSP;
    cJSON * pJsonRoot = NULL;
    cJSON * pduJsonObject = NULL;
    cJSON * devDataObject = NULL;

    pJsonRoot = cJSON_CreateObject();
    if(NULL == pJsonRoot)
    {
        M1_LOG_DEBUG("pJsonRoot NULL\n");
        ret = M1_PROTOCOL_FAILED;
        goto Finish;    
    }

    cJSON_AddNumberToObject(pJsonRoot, "sn", data.sn);
    cJSON_AddStringToObject(pJsonRoot, "version", "1.0");
    cJSON_AddNumberToObject(pJsonRoot, "netFlag", 1);
    cJSON_AddNumberToObject(pJsonRoot, "cmdType", 1);
    /*create pdu object*/
    pduJsonObject = cJSON_CreateObject();
    if(NULL == pduJsonObject)
    {
        cJSON_Delete(pduJsonObject);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;    
    }
    /*add pdu to root*/
    cJSON_AddItemToObject(pJsonRoot, "pdu", pduJsonObject);
    /*add pdu type to pdu object*/
    cJSON_AddNumberToObject(pduJsonObject, "pduType", pduType);
    /*devData*/
    devDataObject = cJSON_CreateObject();
    if(NULL == devDataObject)
    {
        cJSON_Delete(devDataObject);
        ret = M1_PROTOCOL_FAILED;
        goto Finish;    
    }
    /*add devData to pdu*/
    cJSON_AddItemToObject(pduJsonObject, "devData", devDataObject);
    cJSON_AddNumberToObject(devDataObject, "sn", data.sn);
    cJSON_AddNumberToObject(devDataObject, "pduType", data.pduType);
    cJSON_AddNumberToObject(devDataObject, "result", data.result);

    char * p = cJSON_PrintUnformatted(pJsonRoot);
    
    if(NULL == p)
    {    
        ret = M1_PROTOCOL_FAILED;
        goto Finish;    
    }

    M1_LOG_DEBUG("string:%s\n",p);
    /*response to client*/
    socketSeverSend((uint8*)p, strlen(p), data.clientFd);

    Finish:
    cJSON_Delete(pJsonRoot);

    return ret;
}

extern sqlite3* db;
void delete_account_conn_info(int clientFd)
{
    int rc;

    // rc = sql_open();   
    // if( rc != SQLITE_OK){  
    //     M1_LOG_ERROR( "Can't open database: %s\n", sqlite3_errmsg(db));  
    //     return;
    // }else{  
    //     M1_LOG_DEBUG( "Opened database successfully\n");  
    // }

    char* sql = (char*)malloc(300);
    //sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;

    sprintf(sql,"delete from account_info where CLIENT_FD = %03d;",clientFd);
    M1_LOG_DEBUG("string:%s\n",sql);
    if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        goto Finish; 
    }
    thread_sqlite3_step(&stmt, db);

    /*删除链接信息*/
    sprintf(sql,"delete from conn_info where CLIENT_FD = %03d;",clientFd);
    M1_LOG_DEBUG("string:%s\n",sql);
    sqlite3_finalize(stmt);
    if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        goto Finish; 
    }
    thread_sqlite3_step(&stmt, db);
    
    Finish:
    free(sql);
    if(stmt)
        sqlite3_finalize(stmt);

    // sql_close();

}

static void delete_client_db(void)
{
    int clientFd = 0;
    while(fifo_read(&client_delete_fifo, &clientFd))
    {
        delete_account_conn_info(clientFd);
    }
}

/*app修改设备名称*/
static int app_change_device_name(payload_t data)
{
    int rc, ret = M1_PROTOCOL_OK;
    char* errorMsg = NULL;
    char* sql = (char*)malloc(300);
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;
    cJSON* devIdObject = NULL;
    cJSON* devNameObject = NULL;

    if(data.pdu == NULL){
        M1_LOG_ERROR("data NULL\n");
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    db = data.db;   

    /*获取devId*/
    devIdObject = cJSON_GetObjectItem(data.pdu, "devId");
    if(devIdObject == NULL){
        M1_LOG_ERROR("devIdObject NULL\n");
        ret = M1_PROTOCOL_FAILED;
        goto Finish;   
    }
    M1_LOG_DEBUG("devId:%s\n",devIdObject->valuestring);
    devNameObject = cJSON_GetObjectItem(data.pdu, "devName");   
    if(devNameObject == NULL){
        ret = M1_PROTOCOL_FAILED;
        goto Finish;
    }
    M1_LOG_DEBUG("devName:%s\n",devNameObject->valuestring);

    /*修改all_dev设备名称*/
    sprintf(sql,"update all_dev set DEV_NAME = \"%s\" where DEV_ID = \"%s\";",devNameObject->valuestring, devIdObject->valuestring);
    M1_LOG_DEBUG("sql:%s\n",sql);
    if(sqlite3_exec(db, "BEGIN", NULL, NULL, &errorMsg)==SQLITE_OK){
        M1_LOG_DEBUG("BEGIN:\n");
        sqlite3_finalize(stmt);
        if(sqlite3_prepare_v2(db, sql, strlen(sql), &stmt, NULL) != SQLITE_OK){
            M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
            ret = M1_PROTOCOL_FAILED;
            goto Finish; 
        }
        rc = thread_sqlite3_step(&stmt, db);
        if(rc == SQLITE_ERROR)
        {
            ret = M1_PROTOCOL_FAILED;
            goto Finish;   
        }
        if(sqlite3_exec(db, "COMMIT", NULL, NULL, &errorMsg) == SQLITE_OK){
            M1_LOG_DEBUG("END\n");
        }else{
            M1_LOG_DEBUG("ROLLBACK\n");
            if(sqlite3_exec(db, "ROLLBACK", NULL, NULL, &errorMsg) == SQLITE_OK){
                M1_LOG_DEBUG("ROLLBACK OK\n");
                sqlite3_free(errorMsg);
            }else{
                M1_LOG_ERROR("ROLLBACK FALIED\n");
            }
        }
    }else{
        M1_LOG_ERROR("errorMsg\n");
    }

    Finish:
    free(sql);
    sqlite3_finalize(stmt);

    return ret;
}


void getNowTime(char* _time)
{

    struct tm nowTime;

    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);  //获取相对于1970到现在的秒数
    localtime_r(&time.tv_sec, &nowTime);
    
    sprintf(_time, "%04d%02d%02d%02d%02d%02d", nowTime.tm_year + 1900, nowTime.tm_mon+1, nowTime.tm_mday, 
      nowTime.tm_hour, nowTime.tm_min, nowTime.tm_sec);
}

/*Hex to int*/
static uint8_t hex_to_uint8(int h)
{
    if( (h>='0' && h<='9') ||  (h>='a' && h<='f') ||  (h>='A' && h<='F') )
        h += 9*(1&(h>>6));
    else{
        return 0;
    }

    return (uint8_t)(h & 0xf);
}

void setLocalTime(char* time)
{
    struct tm local_tm, *time_1;
    struct timeval tv;
    struct timezone tz;
    int mon, day,hour,min;

    gettimeofday(&tv, &tz);
    mon = 10;
    day = 31;
    hour = 17; 
    min = 55;
    printf("setLocalTime,time:%02d-%02d %02d:%02d\n",mon+1,day,hour,min);
    local_tm.tm_year = 2017 - 1900;
    local_tm.tm_mon = mon;
    local_tm.tm_mday = day;
    local_tm.tm_hour = hour;
    local_tm.tm_min = min;
    local_tm.tm_sec = 30;
    tv.tv_sec = mktime(&local_tm);
    tv.tv_usec = 0;
    settimeofday(&tv, &tz);
}

void delay_send(cJSON* d, int delay, int clientFd)
{
    Item item;

    item.data = d;
    item.prio = delay;
    item.clientFd = clientFd;
    Push(&head, item);
}

void delay_send_task(void)
{
    static uint32_t count = 0;
    Item item;
    char * p = NULL;
    while(1){
        if(!IsEmpty(&head)){
            if(head.next->item.prio <= 0){
                Pop(&head, &item);
                p = item.data;
                //M1_LOG_INFO("delay_send_task data:%s\n",p);
                M1_LOG_WARN("delay_send_task data:%s\n",p);
                socketSeverSend((uint8*)p, strlen(p), item.clientFd);
            }
        }
        usleep(100000);
        count++;
        if(!(count % 10)){
           // count = 0;
            Queue_delay_decrease(&head);
        }
#if TCP_CLIENT_ENABLE
        /*M1心跳到云端*/
        if(!(count % 300))
            m1_heartbeat_to_cloud();
#endif
    }
}

int sql_id(sqlite3* db, char* sql)
{
    M1_LOG_DEBUG("sql_id\n");
    sqlite3_stmt* stmt = NULL;
    int id, total_column, rc;
    /*get id*/
    if(sqlite3_prepare_v2(db, sql, strlen(sql), & stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        goto Finish; 
    }
    rc = thread_sqlite3_step(&stmt, db);
    if(rc == SQLITE_ROW){
            id = (sqlite3_column_int(stmt, 0) + 1);
    }else{
        id = 1;
    }

    Finish:
    sqlite3_finalize(stmt);
    return id;
}

int sql_row_number(sqlite3* db, char*sql)
{
    char** p_result;
    char* errmsg;
    int n_row, n_col, rc;
    rc = sqlite3_get_table(db, sql, &p_result,&n_row, &n_col, &errmsg);
    if(rc != SQLITE_OK)
        M1_LOG_ERROR("sql_row_number failed:%s\n",errmsg);

    sqlite3_free(errmsg);
    sqlite3_free_table(p_result);

    return n_row;
}

int sql_exec(sqlite3* db, char*sql)
{
    sqlite3_stmt* stmt = NULL;
    int rc;
    /*get id*/
    if(sqlite3_prepare_v2(db, sql, strlen(sql), & stmt, NULL) != SQLITE_OK){
        M1_LOG_ERROR( "sqlite3_prepare_v2:error %s\n", sqlite3_errmsg(db));  
        goto Finish; 
    }
    do{
        rc = thread_sqlite3_step(&stmt, db);
    }while(rc == SQLITE_ROW);
   
    Finish:
    sqlite3_finalize(stmt);
    return rc;
}

/*打开数据库*/
int sql_open(void)
{
    int rc;

    pthread_mutex_lock(&mutex_lock);
    M1_LOG_DEBUG( "pthread_mutex_lock\n");

    rc = sqlite3_open(db_path, &db);
    if( rc != SQLITE_OK){  
        M1_LOG_ERROR( "Can't open database\n");  
    }else{  
        M1_LOG_DEBUG( "Opened database successfully\n");  
    }

    return rc;
}

/*关闭数据库*/
int sql_close(void)
{
    int rc;
    
    M1_LOG_DEBUG( "Sqlite3 close\n");
    rc = sqlite3_close(db);

    pthread_mutex_unlock(&mutex_lock);
    M1_LOG_DEBUG( "pthread_mutex_unlock\n");

    return rc;
}

int thread_sqlite3_step(sqlite3_stmt** stmt, sqlite3* db)
{
    int sleep_acount = 0;
    int rc;
    char* errorMsg = NULL;

    rc = sqlite3_step(*stmt);   
    M1_LOG_DEBUG("step() return %s, number:%03d\n", rc == SQLITE_DONE ? "SQLITE_DONE": rc == SQLITE_ROW ? "SQLITE_ROW" : "SQLITE_ERROR",rc);
    if((rc != SQLITE_ROW) && (rc != SQLITE_DONE)){
        M1_LOG_ERROR("step() return %s, number:%03d\n", "SQLITE_ERROR",rc);
    }

    if(rc == SQLITE_BUSY || rc == SQLITE_MISUSE || rc == SQLITE_LOCKED){
        if(sqlite3_exec(db, "ROLLBACK", NULL, NULL, &errorMsg) == SQLITE_OK){
            M1_LOG_INFO("ROLLBACK OK\n");
            sqlite3_free(errorMsg);
        }else{
            sqlite3_free(errorMsg);
            M1_LOG_ERROR("ROLLBACK FALIED\n");
        }
    }else if(rc == SQLITE_CORRUPT){
        M1_LOG_ERROR("SQLITE FATAL ERROR!\n");
        system(sql_back_path);
        exit(0);
    }

    return rc;
}

static int create_sql_table(void)
{
    char* sql = (char*)malloc(600);
    int rc,ret = M1_PROTOCOL_OK;
    char* errmsg = NULL;

    sqlite3* db = 0;
    rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if( rc ){  
        M1_LOG_ERROR( "Can't open database: %s\n", sqlite3_errmsg(db));  
        goto Finish;
        ret = M1_PROTOCOL_FAILED;
    }else{  
        M1_LOG_DEBUG( "Opened database successfully\n");  
    }
    /*account_info*/
    sprintf(sql,"create table account_info(ID INT PRIMARY KEY NOT NULL, ACCOUNT TEXT NOT NULL, CLIENT_FD INT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("account_info already exit: %s\n",errmsg);
        sprintf(sql,"delete from account_info where ID > 0;");
         rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
        if(rc != SQLITE_OK){
            M1_LOG_WARN("delete from account_info failed: %s\n",errmsg);
        }
    }
    sqlite3_free(errmsg);
    /*account_table*/
    sprintf(sql,"create table account_table(ID INT PRIMARY KEY NOT NULL, ACCOUNT TEXT NOT NULL, KEY TEXT NOT NULL,KEY_AUTH TEXT NOT NULL,REMOTE_AUTH TEXT NOT NULL,TIME TEXT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("account_table already exit: %s\n",errmsg);
    }
    sqlite3_free(errmsg);
    /*all_dev*/
    sprintf(sql,"create table all_dev(ID INT PRIMARY KEY NOT NULL, DEV_ID TEXT NOT NULL, DEV_NAME TEXT NOT NULL,AP_ID TEXT NOT NULL,PID INT NOT NULL,ADDED INT NOT NULL,NET INT NOT NULL, STATUS TEXT NOT NULL, ACCOUNT TEXT NOT NULL, TIME TEXT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("all_dev already exit: %s\n",errmsg);
    }
    sqlite3_free(errmsg);
    /*conn_info*/
    sprintf(sql,"create table conn_info(ID INT PRIMARY KEY NOT NULL, AP_ID TEXT NOT NULL, CLIENT_FD INT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("conn_info already exit: %s\n",errmsg);
        sprintf(sql,"delete from conn_info where ID > 0;");
        rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
        if(rc != SQLITE_OK){
            M1_LOG_WARN("delete from conn_info failed: %s\n",errmsg);
        }
    }
    sqlite3_free(errmsg);
    /*district_table*/
    sprintf(sql,"CREATE TABLE district_table(ID INT PRIMARY KEY NOT NULL, DIS_NAME TEXT NOT NULL, DIS_PIC TEXT NOT NULL, AP_ID TEXT NOT NULL, ACCOUNT TEXT NOT NULL, TIME TEXT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("district_table already exit: %s\n",errmsg);
    }
    sqlite3_free(errmsg);
    /*link_exec_table*/
    sprintf(sql,"CREATE TABLE link_exec_table(ID INT PRIMARY KEY NOT NULL, LINK_NAME TEXT NOT NULL, DISTRICT TEXT NOT NULL, AP_ID TEXT NOT NULL, DEV_ID TEXT NOT NULL, TYPE INT NOT NULL, VALUE INT NOT NULL, DELAY INT NOT NULL, TIME TEXT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("link_exec_table already exit: %s\n",errmsg);
    }
    sqlite3_free(errmsg);
    /*link_trigger_table*/
    sprintf(sql,"CREATE TABLE link_trigger_table(ID INT PRIMARY KEY NOT NULL, LINK_NAME TEXT NOT NULL,DISTRICT TEXT NOT NULL, AP_ID TEXT NOT NULL, DEV_ID TEXT NOT NULL, TYPE INT NOT NULL, THRESHOLD INT NOT NULL,CONDITION TEXT NOT NULL, LOGICAL TEXT NOT NULL, STATUS TEXT NOT NULL, TIME TEXT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("link_trigger_table already exit: %s\n",errmsg);
    }
    sqlite3_free(errmsg);
    /*linkage_table*/
    sprintf(sql,"CREATE TABLE linkage_table(ID INT PRIMARY KEY NOT NULL, LINK_NAME TEXT NOT NULL, DISTRICT TEXT NOT NULL, EXEC_TYPE TEXT NOT NULL, EXEC_ID TEXT NOT NULL, STATUS TEXT NOT NULL, ENABLE TEXT NOT NULL, TIME TEXT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("linkage_table already exit: %s\n",errmsg);
    }
    sqlite3_free(errmsg);
    /*param_table*/
    sprintf(sql,"CREATE TABLE param_table(ID INT PRIMARY KEY NOT NULL, DEV_ID TEXT NOT NULL, DEV_NAME TEXT NOT NULL, TYPE INT NOT NULL, VALUE INT NOT NULL, TIME TEXT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("param_table already exit: %s\n",errmsg);
    }
    sqlite3_free(errmsg);
    /*scen_alarm_table*/
    sprintf(sql,"CREATE TABLE scen_alarm_table(ID INT PRIMARY KEY NOT NULL, SCEN_NAME TEXT NOT NULL, HOUR INT NOT NULL,MINUTES INT NOT NULL, WEEK TEXT NOT NULL, STATUS TEXT NOT NULL, TIME TEXT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("scen_alarm_table already exit: %s\n",errmsg);
    }
    sqlite3_free(errmsg);
    /*scenario_table*/
    sprintf(sql,"CREATE TABLE scenario_table(ID INT PRIMARY KEY NOT NULL, SCEN_NAME TEXT NOT NULL, SCEN_PIC TEXT NOT NULL, DISTRICT TEXT NOT NULL, AP_ID TEXT NOT NULL, DEV_ID TEXT NOT NULL, TYPE INT NOT NULL, VALUE INT NOT NULL, DELAY INT NOT NULL, ACCOUNT TEXT NOT NULL, TIME TEXT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("scenario_table already exit: %s\n",errmsg);
    }
    sqlite3_free(errmsg);
    /*project_table*/
    sprintf(sql,"CREATE TABLE project_table(ID INT PRIMARY KEY NOT NULL, P_NAME TEXT NOT NULL, P_NUMBER TEXT NOT NULL, P_CREATOR TEXT NOT NULL, P_MANAGER TEXT NOT NULL, P_EDITOR TEXT NOT NULL, P_TEL TEXT NOT NULL, P_ADD TEXT NOT NULL, P_BRIEF TEXT NOT NULL, P_KEY TEXT NOT NULL, ACCOUNT TEXT NOT NULL, TIME TEXT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("project_table already exit: %s\n",errmsg);
    }
    sqlite3_free(errmsg);
    /*param_detail_table*/
    sprintf(sql,"CREATE TABLE param_detail_table(ID INT PRIMARY KEY NOT NULL, DEV_ID TEXT NOT NULL,  TYPE INT NOT NULL, VALUE INT NOT NULL, DESCRIP TEXT NOT NULL, TIME TEXT NOT NULL);");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("param_detail_table already exit: %s\n",errmsg);
    }
    sqlite3_free(errmsg);
    /*插入Dalitek账户*/
    sprintf(sql,"insert into account_table(ID, ACCOUNT, KEY, KEY_AUTH, REMOTE_AUTH, TIME)values(1,\"Dalitek\",\"root\",\"on\",\"on\",\"20171023110000\");");
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("insert into account_table fail: %s\n",errmsg);
    }
    sqlite3_free(errmsg);
    /*插入项目信息*/
    char* mac_addr = NULL;
    mac_addr = get_eth0_mac_addr();
    sprintf(sql,"insert into project_table(ID, P_NAME, P_NUMBER, P_CREATOR, P_MANAGER, P_EDITOR, P_TEL, P_ADD, P_BRIEF, P_KEY, ACCOUNT, TIME)values(1,\"M1\",\"%s\",\"Dalitek\",\"Dalitek\",\"Dalitek\",\"123456789\",\"ShangHai\",\"Brief\",\"123456\",\"Dalitek\",\"20171031161900\");",mac_addr);
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if(rc != SQLITE_OK){
        M1_LOG_WARN("insert into project_table fail: %s\n",errmsg);
    }
    sqlite3_free(errmsg);

    Finish:
    free(sql);
    sqlite3_close(db);

    return ret;
}


