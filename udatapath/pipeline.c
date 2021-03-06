/* 
 * This file is part of the HDDP Switch distribution (https://github.com/gistnetserv-uah/HDDP).
 * Copyright (c) 2020.
 * 
 * This program is free software: you can redistribute it and/or modify  
 * it under the terms of the GNU General Public License as published by  
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but 
 * WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/types.h>
#include <stdbool.h>
#include <stdlib.h>

#include "action_set.h"
#include "compiler.h"
#include "dp_actions.h"
#include "dp_buffers.h"
#include "dp_exp.h"
#include "dp_ports.h"
#include "datapath.h"
#include "packet.h"
#include "pipeline.h"
#include "flow_table.h"
#include "flow_entry.h"
#include "meter_table.h"
#include "oflib/ofl.h"
#include "oflib/ofl-structs.h"
#include "util.h"
#include "hash.h"
#include "oflib/oxm-match.h"
#include "vlog.h"


#define LOG_MODULE VLM_pipeline

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(60, 60);

static void
execute_entry(struct pipeline *pl, struct flow_entry *entry,
              struct flow_table **table, struct packet **pkt);

struct pipeline *
pipeline_create(struct datapath *dp) {
    struct pipeline *pl;
    int i;
    pl = xmalloc(sizeof(struct pipeline));
    for (i=0; i<PIPELINE_TABLES; i++) {
        pl->tables[i] = flow_table_create(dp, i);
    }
    pl->dp = dp;
    nblink_initialize();
    return pl;
}

static bool
is_table_miss(struct flow_entry *entry){
    return ((entry->stats->priority) == 0 && (entry->match->length <= 4));
}

/* Sends a packet to the controller in a packet_in message */
static void
send_packet_to_controller(struct pipeline *pl, struct packet *pkt, uint8_t table_id, uint8_t reason) {

    struct ofl_msg_packet_in msg;
    struct ofl_match *m;
    msg.header.type = OFPT_PACKET_IN;
    msg.total_len   = pkt->buffer->size;
    msg.reason      = reason;
    msg.table_id    = table_id;
    msg.cookie      = 0xffffffffffffffff;
    msg.data = pkt->buffer->data;


    /* A max_len of OFPCML_NO_BUFFER means that the complete
        packet should be sent, and it should not be buffered.*/
    if (pl->dp->config.miss_send_len != OFPCML_NO_BUFFER){
        dp_buffers_save(pl->dp->buffers, pkt);
        msg.buffer_id   = pkt->buffer_id;
        msg.data_length = MIN(pl->dp->config.miss_send_len, pkt->buffer->size);
    }else {
        msg.buffer_id   = OFP_NO_BUFFER;
        msg.data_length = pkt->buffer->size;
    }

    m = &pkt->handle_std->match;
    /* In this implementation the fields in_port and in_phy_port
        always will be the same, because we are not considering logical
        ports                                 */
    msg.match = (struct ofl_match_header*)m;
    dp_send_message(pl->dp, (struct ofl_msg_header *)&msg, NULL);
}

/* Pass the packet through the flow tables.
 * This function takes ownership of the packet and will destroy it. */
void
pipeline_process_packet(struct pipeline *pl, struct packet *pkt) {
    struct flow_table *table, *next_table;

    if (VLOG_IS_DBG_ENABLED(LOG_MODULE)) {
        char *pkt_str = packet_to_string(pkt);
        VLOG_DBG_RL(LOG_MODULE, &rl, "processing packet: %s", pkt_str);
        free(pkt_str);
    }

    if (!packet_handle_std_is_ttl_valid(pkt->handle_std)) {
        send_packet_to_controller(pl, pkt, 0/*table_id*/, OFPR_INVALID_TTL);
        packet_destroy(pkt);
        return;
    }

    /*Modificacion UAH Discovery hybrid topologies, JAH-*/
    //Tratamos los hellos para detectar a los sensores
    if (handle_hello_packets(pkt) == 1){
        VLOG_INFO(LOG_MODULE,"Paquete Hello tratado Correctamente!");
        //una vez tratado eliminamos el mensaje
        if (pkt)
        {
            packet_destroy(pkt);
            VLOG_INFO(LOG_MODULE, "Paquete Hello Eliminado Correctamente!");
        }    
        return;
    }
    //Tratamos los paquetes del protocolo, empezando por el Request (Broadcast)
    if (selecto_HDT_packets(pkt) == 1){
        VLOG_INFO(LOG_MODULE, "Paquete HDT tratado Correctamente!");
        //una vez tratado eliminamos el mensaje
        if (pkt)
        {
            packet_destroy(pkt);
            VLOG_INFO(LOG_MODULE, "Paquete HDT Eliminado Correctamente!");
        }
        return;
    }

    /*Fin Modificacion UAH Discovery hybrid topologies, JAH-*/

    next_table = pl->tables[0];
    while (next_table != NULL) {
        struct flow_entry *entry;

        VLOG_DBG_RL(LOG_MODULE, &rl, "trying table %u.", next_table->stats->table_id);

        pkt->table_id = next_table->stats->table_id;
        table         = next_table;
        next_table    = NULL;

        // EEDBEH: additional printout to debug table lookup
        if (VLOG_IS_DBG_ENABLED(LOG_MODULE)) {
            char *m = ofl_structs_match_to_string((struct ofl_match_header*)&(pkt->handle_std->match), pkt->dp->exp);
            VLOG_DBG_RL(LOG_MODULE, &rl, "searching table entry for packet match: %s.", m);
            free(m);
        }
        entry = flow_table_lookup(table, pkt);
        if (entry != NULL) {
	        if (VLOG_IS_DBG_ENABLED(LOG_MODULE)) {
                char *m = ofl_structs_flow_stats_to_string(entry->stats, pkt->dp->exp);
                VLOG_DBG_RL(LOG_MODULE, &rl, "found matching entry: %s.", m);
                free(m);
            }
            pkt->handle_std->table_miss = is_table_miss(entry);
            execute_entry(pl, entry, &next_table, &pkt);
            /* Packet could be destroyed by a meter instruction */
            if (!pkt)
                return;

            if (next_table == NULL) {
               /* Cookie field is set 0xffffffffffffffff
                because we cannot associate it to any
                particular flow */
                action_set_execute(pkt->action_set, pkt, 0xffffffffffffffff);
                return;
            }

        } else {
			/* OpenFlow 1.3 default behavior on a table miss */
			VLOG_DBG_RL(LOG_MODULE, &rl, "No matching entry found. Dropping packet.");
			packet_destroy(pkt);
			return;
        }
    }
    VLOG_WARN_RL(LOG_MODULE, &rl, "Reached outside of pipeline processing cycle.");
}

static
int inst_compare(const void *inst1, const void *inst2){
    struct ofl_instruction_header * i1 = *(struct ofl_instruction_header **) inst1;
    struct ofl_instruction_header * i2 = *(struct ofl_instruction_header **) inst2;
    if ((i1->type == OFPIT_APPLY_ACTIONS && i2->type == OFPIT_CLEAR_ACTIONS) ||
        (i1->type == OFPIT_CLEAR_ACTIONS && i2->type == OFPIT_APPLY_ACTIONS))
        return i1->type > i2->type;

    return i1->type < i2->type;
}

ofl_err
pipeline_handle_flow_mod(struct pipeline *pl, struct ofl_msg_flow_mod *msg,
                                                const struct sender *sender) {
    /* Note: the result of using table_id = 0xff is undefined in the spec.
     *       for now it is accepted for delete commands, meaning to delete
     *       from all tables */
    ofl_err error;
    size_t i;
    bool match_kept,insts_kept;

    if(sender->remote->role == OFPCR_ROLE_SLAVE)
        return ofl_error(OFPET_BAD_REQUEST, OFPBRC_IS_SLAVE);

    match_kept = false;
    insts_kept = false;

    /*Sort by execution oder*/
    qsort(msg->instructions, msg->instructions_num,
        sizeof(struct ofl_instruction_header *), inst_compare);

    // Validate actions in flow_mod
    for (i=0; i< msg->instructions_num; i++) {
        if (msg->instructions[i]->type == OFPIT_APPLY_ACTIONS ||
            msg->instructions[i]->type == OFPIT_WRITE_ACTIONS) {
            struct ofl_instruction_actions *ia = (struct ofl_instruction_actions *)msg->instructions[i];

            error = dp_actions_validate(pl->dp, ia->actions_num, ia->actions);
            if (error) {
                return error;
            }
            error = dp_actions_check_set_field_req(msg, ia->actions_num, ia->actions);
            if (error) {
                return error;
            }
        }
	/* Reject goto in the last table. */
	if ((msg->table_id == (PIPELINE_TABLES - 1))
	    && (msg->instructions[i]->type == OFPIT_GOTO_TABLE))
	  return ofl_error(OFPET_BAD_INSTRUCTION, OFPBIC_UNSUP_INST);
    }

    if (msg->table_id == 0xff) {
        if (msg->command == OFPFC_DELETE || msg->command == OFPFC_DELETE_STRICT) {
            size_t i;

            error = 0;
            for (i=0; i < PIPELINE_TABLES; i++) {
                error = flow_table_flow_mod(pl->tables[i], msg, &match_kept, &insts_kept);
                if (error) {
                    break;
                }
            }
            if (error) {
                return error;
            } else {
                ofl_msg_free_flow_mod(msg, !match_kept, !insts_kept, pl->dp->exp);
                return 0;
            }
        } else {
            return ofl_error(OFPET_FLOW_MOD_FAILED, OFPFMFC_BAD_TABLE_ID);
        }
    } else {
        error = flow_table_flow_mod(pl->tables[msg->table_id], msg, &match_kept, &insts_kept);
        if (error) {
            return error;
        }
        if ((msg->command == OFPFC_ADD || msg->command == OFPFC_MODIFY || msg->command == OFPFC_MODIFY_STRICT) &&
                            msg->buffer_id != NO_BUFFER) {
            /* run buffered message through pipeline */
            struct packet *pkt;

            pkt = dp_buffers_retrieve(pl->dp->buffers, msg->buffer_id);
            if (pkt != NULL) {
		      pipeline_process_packet(pl, pkt);
            } else {
                VLOG_WARN_RL(LOG_MODULE, &rl, "The buffer flow_mod referred to was empty (%u).", msg->buffer_id);
            }
        }

        ofl_msg_free_flow_mod(msg, !match_kept, !insts_kept, pl->dp->exp);
        return 0;
    }

}

ofl_err
pipeline_handle_table_mod(struct pipeline *pl,
                          struct ofl_msg_table_mod *msg,
                          const struct sender *sender) {

    if(sender->remote->role == OFPCR_ROLE_SLAVE)
        return ofl_error(OFPET_BAD_REQUEST, OFPBRC_IS_SLAVE);

    if (msg->table_id == 0xff) {
        size_t i;

        for (i=0; i<PIPELINE_TABLES; i++) {
            pl->tables[i]->features->config = msg->config;
        }
    } else {
        pl->tables[msg->table_id]->features->config = msg->config;
    }

    ofl_msg_free((struct ofl_msg_header *)msg, pl->dp->exp);
    return 0;
}

ofl_err
pipeline_handle_stats_request_flow(struct pipeline *pl,
                                   struct ofl_msg_multipart_request_flow *msg,
                                   const struct sender *sender) {

    struct ofl_flow_stats **stats = xmalloc(sizeof(struct ofl_flow_stats *));
    size_t stats_size = 1;
    size_t stats_num = 0;

    if (msg->table_id == 0xff) {
        size_t i;
        for (i=0; i<PIPELINE_TABLES; i++) {
            flow_table_stats(pl->tables[i], msg, &stats, &stats_size, &stats_num);
        }
    } else {
        flow_table_stats(pl->tables[msg->table_id], msg, &stats, &stats_size, &stats_num);
    }

    {
        struct ofl_msg_multipart_reply_flow reply =
                {{{.type = OFPT_MULTIPART_REPLY},
                  .type = OFPMP_FLOW, .flags = 0x0000},
                 .stats     = stats,
                 .stats_num = stats_num
                };

        dp_send_message(pl->dp, (struct ofl_msg_header *)&reply, sender);
    }

    free(stats);
    ofl_msg_free((struct ofl_msg_header *)msg, pl->dp->exp);
    return 0;
}

ofl_err
pipeline_handle_stats_request_table(struct pipeline *pl,
                                    struct ofl_msg_multipart_request_header *msg UNUSED,
                                    const struct sender *sender) {
    struct ofl_table_stats **stats;
    size_t i;

    stats = xmalloc(sizeof(struct ofl_table_stats *) * PIPELINE_TABLES);

    for (i=0; i<PIPELINE_TABLES; i++) {
        stats[i] = pl->tables[i]->stats;
    }

    {
        struct ofl_msg_multipart_reply_table reply =
                {{{.type = OFPT_MULTIPART_REPLY},
                  .type = OFPMP_TABLE, .flags = 0x0000},
                 .stats     = stats,
                 .stats_num = PIPELINE_TABLES};

        dp_send_message(pl->dp, (struct ofl_msg_header *)&reply, sender);
    }

    free(stats);
    ofl_msg_free((struct ofl_msg_header *)msg, pl->dp->exp);
    return 0;
}

ofl_err
pipeline_handle_stats_request_table_features_request(struct pipeline *pl,
                                    struct ofl_msg_multipart_request_header *msg,
                                    const struct sender *sender) {
    struct ofl_table_features **features;
    struct ofl_msg_multipart_request_table_features *feat =
                       (struct ofl_msg_multipart_request_table_features *) msg;
    int i;           /* Feature index in feature array. Jean II */
    int table_id;
    ofl_err error = 0;

    /* Further validation of request not done in
     * ofl_structs_table_features_unpack(). Jean II */
    if(feat->table_features != NULL) {
        for(i = 0; i < feat->tables_num; i++){
	    if(feat->table_features[i]->table_id >= PIPELINE_TABLES)
	        return ofl_error(OFPET_TABLE_FEATURES_FAILED, OFPTFFC_BAD_TABLE);
	    /* We may want to validate things like config, max_entries,
	     * metadata... */
        }
    }

    /* Check if we already received fragments of a multipart request. */
    if(sender->remote->mp_req_msg != NULL) {
      bool nomore;

      /* We can only merge requests having the same XID. */
      if(sender->xid != sender->remote->mp_req_xid)
	{
	  VLOG_ERR(LOG_MODULE, "multipart request: wrong xid (0x%X != 0x%X)", sender->xid, sender->remote->mp_req_xid);

	  /* Technically, as our buffer can only hold one pending request,
	   * this is a buffer overflow ! Jean II */
	  /* Return error. */
	  return ofl_error(OFPET_BAD_REQUEST, OFPBRC_MULTIPART_BUFFER_OVERFLOW);
	}

      VLOG_DBG(LOG_MODULE, "multipart request: merging with previous fragments (%zu+%zu)", ((struct ofl_msg_multipart_request_table_features *) sender->remote->mp_req_msg)->tables_num, feat->tables_num);

      /* Merge the request with previous fragments. */
      nomore = ofl_msg_merge_multipart_request_table_features((struct ofl_msg_multipart_request_table_features *) sender->remote->mp_req_msg, feat);

      /* Check if incomplete. */
      if(!nomore)
	return 0;

      VLOG_DBG(LOG_MODULE, "multipart request: reassembly complete (%zu)", ((struct ofl_msg_multipart_request_table_features *) sender->remote->mp_req_msg)->tables_num);

      /* Use the complete request. */
      feat = (struct ofl_msg_multipart_request_table_features *) sender->remote->mp_req_msg;

#if 0
      {
	char *str;
	str = ofl_msg_to_string((struct ofl_msg_header *) feat, pl->dp->exp);
	VLOG_DBG(LOG_MODULE, "\nMerged request:\n%s\n\n", str);
	free(str);
      }
#endif

    } else {
      /* Check if the request is an initial fragment. */
      if(msg->flags & OFPMPF_REQ_MORE) {
	struct ofl_msg_multipart_request_table_features* saved_msg;

	VLOG_DBG(LOG_MODULE, "multipart request: create reassembly buffer (%zu)", feat->tables_num);

	/* Create a buffer the do reassembly. */
	saved_msg = (struct ofl_msg_multipart_request_table_features*) malloc(sizeof(struct ofl_msg_multipart_request_table_features));
	saved_msg->header.header.type = OFPT_MULTIPART_REQUEST;
	saved_msg->header.type = OFPMP_TABLE_FEATURES;
	saved_msg->header.flags = 0;
	saved_msg->tables_num = 0;
	saved_msg->table_features = NULL;

	/* Save the fragment for later use. */
	ofl_msg_merge_multipart_request_table_features(saved_msg, feat);
	sender->remote->mp_req_msg = (struct ofl_msg_multipart_request_header *) saved_msg;
	sender->remote->mp_req_xid = sender->xid;

	return 0;
      }

      /* Non fragmented request. Nothing to do... */
      VLOG_DBG(LOG_MODULE, "multipart request: non-fragmented request (%zu)", feat->tables_num);
    }

    /*Check to see if the body is empty.*/
    /* Should check merge->tables_num instead. Jean II */
    if(feat->table_features != NULL){
        int last_table_id = 0;

	/* Check that the table features make sense. */
        for(i = 0; i < feat->tables_num; i++){
            /* Table-IDs must be in ascending order. */
            table_id = feat->table_features[i]->table_id;
            if(table_id < last_table_id) {
                error = ofl_error(OFPET_TABLE_FEATURES_FAILED, OFPTFFC_BAD_TABLE);
		break;
            }
            /* Can't go over out internal max-entries. */
            if (feat->table_features[i]->max_entries > FLOW_TABLE_MAX_ENTRIES) {
                error = ofl_error(OFPET_TABLE_FEATURES_FAILED, OFPTFFC_BAD_ARGUMENT);
		break;
            }
        }

        if (error == 0) {

            /* Disable all tables, they will be selectively re-enabled. */
            for(table_id = 0; table_id < PIPELINE_TABLES; table_id++){
	        pl->tables[table_id]->disabled = true;
            }
            /* Change tables configuration
               TODO: Remove flows*/
            VLOG_DBG(LOG_MODULE, "pipeline_handle_stats_request_table_features_request: updating features");
            for(i = 0; i < feat->tables_num; i++){
                table_id = feat->table_features[i]->table_id;

                /* Replace whole table feature. */
                ofl_structs_free_table_features(pl->tables[table_id]->features, pl->dp->exp);
                pl->tables[table_id]->features = feat->table_features[i];
                feat->table_features[i] = NULL;

                /* Re-enable table. */
                pl->tables[table_id]->disabled = false;
            }
        }
    }

    /* Cleanup request. */
    if(sender->remote->mp_req_msg != NULL) {
      ofl_msg_free((struct ofl_msg_header *) sender->remote->mp_req_msg, pl->dp->exp);
      sender->remote->mp_req_msg = NULL;
      sender->remote->mp_req_xid = 0;  /* Currently not needed. Jean II. */
    }

    if (error) {
        return error;
    }

    table_id = 0;
    /* Query for table capabilities */
    loop: ;
    features = (struct ofl_table_features**) xmalloc(sizeof(struct ofl_table_features *) * 8);
    /* Return 8 tables per reply segment. */
    for (i = 0; i < 8; i++){
        /* Skip disabled tables. */
        while((table_id < PIPELINE_TABLES) && (pl->tables[table_id]->disabled == true))
	    table_id++;
	/* Stop at the last table. */
	if(table_id >= PIPELINE_TABLES)
	    break;
	/* Use that table in the reply. */
        features[i] = pl->tables[table_id]->features;
        table_id++;
    }
    VLOG_DBG(LOG_MODULE, "multipart reply: returning %d tables, next table-id %d", i, table_id);
    {
    struct ofl_msg_multipart_reply_table_features reply =
         {{{.type = OFPT_MULTIPART_REPLY},
           .type = OFPMP_TABLE_FEATURES,
           .flags = (table_id == PIPELINE_TABLES ? 0x00000000 : OFPMPF_REPLY_MORE) },
          .table_features     = features,
          .tables_num = i };
          dp_send_message(pl->dp, (struct ofl_msg_header *)&reply, sender);
    }
    if (table_id < PIPELINE_TABLES){
           goto loop;
    }
    free(features);

    return 0;
}

ofl_err
pipeline_handle_stats_request_aggregate(struct pipeline *pl,
                                  struct ofl_msg_multipart_request_flow *msg,
                                  const struct sender *sender) {
    struct ofl_msg_multipart_reply_aggregate reply =
            {{{.type = OFPT_MULTIPART_REPLY},
              .type = OFPMP_AGGREGATE, .flags = 0x0000},
              .packet_count = 0,
              .byte_count   = 0,
              .flow_count   = 0};

    if (msg->table_id == 0xff) {
        size_t i;

        for (i=0; i<PIPELINE_TABLES; i++) {
            flow_table_aggregate_stats(pl->tables[i], msg,
                                       &reply.packet_count, &reply.byte_count, &reply.flow_count);
        }

    } else {
        flow_table_aggregate_stats(pl->tables[msg->table_id], msg,
                                   &reply.packet_count, &reply.byte_count, &reply.flow_count);
    }

    dp_send_message(pl->dp, (struct ofl_msg_header *)&reply, sender);

    ofl_msg_free((struct ofl_msg_header *)msg, pl->dp->exp);
    return 0;
}


void
pipeline_destroy(struct pipeline *pl) {
    struct flow_table *table;
    int i;

    for (i=0; i<PIPELINE_TABLES; i++) {
        table = pl->tables[i];
        if (table != NULL) {
            flow_table_destroy(table);
        }
    }
    free(pl);
}


void
pipeline_timeout(struct pipeline *pl) {
    int i;

    for (i = 0; i < PIPELINE_TABLES; i++) {
        flow_table_timeout(pl->tables[i]);
    }
}


/* Executes the instructions associated with a flow entry */
static void
execute_entry(struct pipeline *pl, struct flow_entry *entry,
              struct flow_table **next_table, struct packet **pkt) {
    /* NOTE: instructions, when present, will be executed in
            the following order:
            Meter
            Apply-Actions
            Clear-Actions
            Write-Actions
            Write-Metadata
            Goto-Table
    */
    size_t i;
    struct ofl_instruction_header *inst;

    for (i=0; i < entry->stats->instructions_num; i++) {
        /*Packet was dropped by some instruction or action*/

        if(!(*pkt)){
            return;
        }

        inst = entry->stats->instructions[i];
        switch (inst->type) {
            case OFPIT_GOTO_TABLE: {
                struct ofl_instruction_goto_table *gi = (struct ofl_instruction_goto_table *)inst;

                *next_table = pl->tables[gi->table_id];
                break;
            }
            case OFPIT_WRITE_METADATA: {
                struct ofl_instruction_write_metadata *wi = (struct ofl_instruction_write_metadata *)inst;
                struct  ofl_match_tlv *f;

                /* NOTE: Hackish solution. If packet had multiple handles, metadata
                 *       should be updated in all. */
                packet_handle_std_validate((*pkt)->handle_std);
                /* Search field on the description of the packet. */
                HMAP_FOR_EACH_WITH_HASH(f, struct ofl_match_tlv,
                    hmap_node, hash_int(OXM_OF_METADATA,0), &(*pkt)->handle_std->match.match_fields){
                    uint64_t *metadata = (uint64_t*) f->value;
                    *metadata = (*metadata & ~wi->metadata_mask) | (wi->metadata & wi->metadata_mask);
                    VLOG_DBG_RL(LOG_MODULE, &rl, "Executing write metadata: 0x%"PRIx64"", *metadata);
                }
                break;
            }
            case OFPIT_WRITE_ACTIONS: {
                struct ofl_instruction_actions *wa = (struct ofl_instruction_actions *)inst;
                action_set_write_actions((*pkt)->action_set, wa->actions_num, wa->actions);
                break;
            }
            case OFPIT_APPLY_ACTIONS: {
                struct ofl_instruction_actions *ia = (struct ofl_instruction_actions *)inst;
                dp_execute_action_list((*pkt), ia->actions_num, ia->actions, entry->stats->cookie);
                break;
            }
            case OFPIT_CLEAR_ACTIONS: {
                action_set_clear_actions((*pkt)->action_set);
                break;
            }
            case OFPIT_METER: {
            	struct ofl_instruction_meter *im = (struct ofl_instruction_meter *)inst;
                meter_table_apply(pl->dp->meters, pkt , im->meter_id);
                break;
            }
            case OFPIT_EXPERIMENTER: {
                dp_exp_inst((*pkt), (struct ofl_instruction_experimenter *)inst);
                break;
            }
        }
    }
}

/*Modificacion UAH Discovery hybrid topologies, JAH-*/

int8_t handle_hello_packets(struct packet *pkt){
    uint16_t  eth_type = pkt->handle_std->proto->eth->eth_type, type_device = 1;

    //packet Hello (EthType = 7698 o 9876)
    if( eth_type== 30360 || eth_type == 39030)
    {
        //Solo se entra para guardar info si no existe conexion entre sensores y es el 
        //gateway quien tiene que descubrir a los sensores
        if (SENSOR_TO_SENSOR == 0){
            //sacamos el tipo de sensores que nos llega
            memcpy(&(type_device), ofpbuf_at_assert(pkt->buffer, pkt->buffer->size - 46*sizeof(uint8_t), 
                sizeof(uint16_t)), sizeof(uint16_t));
            //pasamos a realizar la actualizacion de la tabla de vecinos
            if(mac_to_port_found_port(&neighbor_table, pkt->handle_std->proto->eth->eth_src) != -1)
                mac_to_port_update(&neighbor_table, pkt->handle_std->proto->eth->eth_src, htons(type_device), pkt->in_port, TIME_HELLO_SENSOR);
            else
                //guardamos el valor del vecino
                mac_to_port_add(&neighbor_table, pkt->handle_std->proto->eth->eth_src, htons(type_device), pkt->in_port, TIME_HELLO_SENSOR);
            //paquete tratado correctamente;
        }
        return 1;
    }
    //no se trata el paquete por lo tanto retornamos 0
    return 0; 
}

uint8_t selecto_HDT_packets(struct packet *pkt){
    uint16_t  eth_type = pkt->handle_std->proto->eth->eth_type;

    //packet HDT (EthType = FFAA o AAFF)
    if( eth_type== 65450 || eth_type == 43775 ){
        if ((SENSOR_TO_SENSOR == 1) || (SENSOR_TO_SENSOR == 0 && pkt->dp->id < 0x0100)){   
            //paquetes broadcast son paquetes request
            VLOG_INFO(LOG_MODULE, "Paquete DHT detectado Opcode : %d", htons(pkt->handle_std->proto->dht->opcode));
            if (htons(pkt->handle_std->proto->dht->opcode) == 1){
                VLOG_INFO(LOG_MODULE, "Paquete DHT REQUEST detectado");
                handle_hdt_request_packets(pkt);
            }//paquetes unicast son paquetes reply
            else if (htons(pkt->handle_std->proto->dht->opcode) == 2){
                VLOG_INFO(LOG_MODULE, "Paquete DHT REPLY detectado");
                handle_hdt_reply_packets(pkt);
            }
            else{
                VLOG_INFO(LOG_MODULE, "Paquete DHT DEFECTUOSO!!!");
            }
        }
        //el paquete se ha tratado como un HDT
        return 1;
    }
    //el paquete no es un HDT se debe tratar por otro lado
    return 0;
}

uint8_t handle_hdt_request_packets(struct packet *pkt){
    int table_port = 0; //varible auxiliar para puertp
    int response_reply = 1; //variable para responder o no con reply
    int num_ports = 0; //numero de puertos disponibles (son todos menos los utilizados por los sensores y caidos)

    /*Contamos numero de paquetes request*/
    log_count_request_pks("Request Detectado\n");

    VLOG_INFO(LOG_MODULE, "Calculamos el puerto de entrada y el numero de puertos disponible");
    num_ports = num_port_available(&neighbor_table, pkt->dp);
    table_port = mac_to_port_found_port(&bt_table, pkt->handle_std->proto->eth->eth_src);
    VLOG_INFO(LOG_MODULE, "Puerto: entrada %d | Puerto en tabla: %d | Numero de puertos disponibles: %d", 
        pkt->in_port, table_port ,num_ports);

    if (table_port == -1 ) //Puerto no encontrado
    {
        VLOG_INFO(LOG_MODULE, "Anyado entrada a la tabla de bloqueo: %d", pkt->in_port);
        mac_to_port_add(&bt_table, pkt->handle_std->proto->eth->eth_src, 1, pkt->in_port, BT_TIME);
        response_reply = 0; 
    }
    else if (table_port == 0 ) //puerto encontrado pero caducado
    {
        VLOG_INFO(LOG_MODULE, "actualizo el puerto de la entrada de tabla BT al puerto: %d", pkt->in_port);
        mac_to_port_update(&bt_table, pkt->handle_std->proto->eth->eth_src, 1, pkt->in_port, BT_TIME);
        response_reply = 0; 
    }
    else if (table_port == pkt->in_port){ //Puerto encontrado y valido, comparamos con el de entrada
        VLOG_INFO(LOG_MODULE, "actualizo el tiempo de la entrada de tabla BT");
        mac_to_port_time_refresh(&bt_table, pkt->handle_std->proto->eth->eth_src, BT_TIME);
        response_reply = 0; 
    } 
    
    /* Si solo tengo un puerto contesto con reply */
    if (num_ports == 1 || response_reply == 1){
        VLOG_INFO(LOG_MODULE, "Entramos en generar el reply: num_ports: %d | response_reply: %d", num_ports, response_reply);
        VLOG_INFO(LOG_MODULE, "Request detectado pasamos a crear los Replys de contestacion");
        log_uah("Envio Replies:\n", pkt->dp->id);
        visualizar_tabla(&bt_table, pkt->dp->id);
        creator_dht_reply_packets(pkt);
        VLOG_INFO(LOG_MODULE, "Replies enviado pasamos eliminar el Request");
        return 1;
    }
    //actualizo el numero de dispositivos por los que pasa el request, solo si tengo puertos
    //disponibles para reenviar el paquete
    else {
        update_data_request(pkt);
        VLOG_INFO(LOG_MODULE,"Update number of jump: %d ",
            bigtolittle16(pkt->handle_std->proto->dht->num_devices));
        log_uah("Envio Request:\n", pkt->dp->id);
        visualizar_tabla(&bt_table, pkt->dp->id);
        dp_actions_output_port(pkt, OFPP_FLOOD, pkt->out_queue, pkt->out_port_max_len, 0xffffffffffffffff);
        return 1;
    }
    return 0;
}

uint8_t handle_hdt_reply_packets(struct packet *pkt){

    uint32_t out_port = mac_to_port_found_port(&bt_table, pkt->handle_std->proto->eth->eth_dst);
    uint16_t num_elementos = 0;
    uint16_t type_device = 1;

    if (out_port < 1){
        VLOG_INFO(LOG_MODULE,"ERROR!!!! DON'T found any out_port!!!!");
        return 0;
    }
    else
    {
         /** Soy un sensor indico que sensor soy */
        if (pkt->dp->id >= 0x1000 )
            type_device = type_sensor;
        else /** Como no soy un sensor, soy un NO SDN */
            type_device = NODO_NO_SDN; 

        VLOG_INFO(LOG_MODULE, "Update the hdt reply packet: in-port: %d | out-port %d | type_device: %d",
            pkt->in_port, out_port, type_device);

        if (pkt->in_port == out_port){
            VLOG_INFO(LOG_MODULE, "ERROR!!! El puerto de entrada y salida no pueden ser iguales para un paquete unicast");
            log_uah("ERROR!!! El puerto de entrada y salida no pueden ser iguales para un paquete unicast\n",pkt->dp->id);
            visualizar_tabla(&bt_table, pkt->dp->id);
            return 0;
        }

        num_elementos = update_data_reply(pkt, out_port, type_device);
        VLOG_INFO(LOG_MODULE, "Reply packet: %s", packet_to_string(pkt));

        if(num_elementos == 0){ // Indica qeu tenemos hueco en el paquete para enviar 
            //visualizar_tabla(mac_port, pkt->dp->id);
            dp_actions_output_port(pkt, out_port, pkt->out_queue, pkt->out_port_max_len, 0xffffffffffffffff);
        }
        else
            VLOG_INFO(LOG_MODULE, "Se ha sobrepasado el numero de elementos maximos en el paquete: %d"
                ,num_elementos);
                
        return 1;
    }    
}


void creator_dht_reply_packets(struct packet *pkt){
    uint32_t out_port = 0;
    uint64_t pos_sensor = 0;
    struct packet *pkt_reply = NULL;
    uint8_t Mac[ETH_ADDR_LEN]={0};
    uint16_t type_device = 1;

    if (neighbor_table.num_element == 0 || (SENSOR_TO_SENSOR == 1)) // no tengo sensores solo mando mi información
    {
        VLOG_INFO(LOG_MODULE, "neighbor_table.num_element == 0");
        //en este caso el puerto salida y el puerto de entrada es el mismo ya que contesto a un reply
        //genero el paquete de respuesta
        if (pkt->dp->id >= 0x1000 ){ /** Soy un sensor indico que sensor soy */
            type_device = type_sensor;
            VLOG_INFO(LOG_MODULE, "Son un sensor tipo :%d",type_device);
        }
        else /** Como no soy un sensor, soy un NO SDN */
        {
            type_device = NODO_NO_SDN; 
            VLOG_INFO(LOG_MODULE, "Son un NO SDN:%d",NODO_NO_SDN);
        }
        VLOG_INFO(LOG_MODULE, "Numero de elementos: %d", (int)(neighbor_table.num_element + 1));
        pkt_reply = create_dht_reply_packet(pkt->dp, pkt->handle_std->proto->eth->eth_src,pkt->in_port,
            pkt->in_port, type_device, mac2int(pkt->dp->ports[1].conf->hw_addr), (uint16_t)(neighbor_table.num_element + 1));
        VLOG_INFO(LOG_MODULE, "create_dht_reply_packet OK");
        //envio el paquete por el puerto de entrada
        dp_actions_output_port(pkt_reply, pkt->in_port, pkt->out_queue, pkt->out_port_max_len, 0xffffffffffffffff);
        VLOG_INFO(LOG_MODULE, "dp_actions_output_port OK OUT PORT : %d", pkt->in_port);
        //destruyo el paquete para limpiar la memoria
        if (pkt_reply){
            packet_destroy(pkt_reply);
            VLOG_INFO(LOG_MODULE, "pkt_reply delete!! OK");
        }
        
    }
    else {
        //debemos generar todos los paquetes replies que pertenecen a un mismo request
        VLOG_INFO(LOG_MODULE, "Tenemos sensores que transmitir!!!-> %d", (int)neighbor_table.num_element);
        for (pos_sensor = 0; pos_sensor < neighbor_table.num_element; pos_sensor ++){
            VLOG_INFO(LOG_MODULE, "comenzamos con el sensor %lu",pos_sensor);
            //buscamos el puerto de salida para llegar al controller
            out_port = mac_to_port_found_port_position(&neighbor_table, pos_sensor);
            type_device = mac_to_port_found_mac_position(&neighbor_table, pos_sensor, Mac); 
            VLOG_INFO(LOG_MODULE, "Puerto de conexion el sensor: %d",(int)out_port);
            VLOG_INFO(LOG_MODULE, "Sensory type : %d", (int)type_device);
            //solo nos valen los sensores que no hayan caducado
            if (out_port > 0 && type_device > 0){
                //Creamos el paquete con la información del sensore y el nodo
                pkt_reply = create_dht_reply_packet(pkt->dp, pkt->handle_std->proto->eth->eth_src,
                    out_port, pkt->in_port, type_device, mac2int(Mac), (uint16_t)neighbor_table.num_element);
                VLOG_INFO(LOG_MODULE, "Send Reply packet with sensor information!!!!!!");
                //una vez localizado el puerto conexion con el sensor, el puerto de salida del mensaje
                //es el puerto de entrada del request
                out_port = pkt->in_port;
                VLOG_INFO(LOG_MODULE, "Send via in port: %d", out_port);
                //enviamos el paquete
                dp_actions_output_port(pkt_reply, out_port, pkt->out_queue, pkt->out_port_max_len, 0xffffffffffffffff);
                VLOG_INFO(LOG_MODULE, "Send via in port: %d", out_port);
                //destruyo el paquete para limpiar la memoria
                if (pkt_reply){
                    packet_destroy(pkt_reply);
                    VLOG_INFO(LOG_MODULE, "pkt_reply delete!! OK");
                }
            }
            else
            {
                VLOG_INFO(LOG_MODULE, "Sensor NOT VALID!!");
            }
            
        }
    }
    
}

/*Fin Modificacion UAH Discovery hybrid topologies, JAH-*/
