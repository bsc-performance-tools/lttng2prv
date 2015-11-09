#include "ctf2prv.h"

static char *opt_output;
const char *inputTrace;

uint64_t offset;

short int verbose = 0;

static void print_usage(FILE *fp)
{
	fprintf(fp, "CTF2PRV trace converter \n\n");
	fprintf(fp, "Usage: ctf2prv [OPTIONS] FILE\n");
	fprintf(fp, "\tFILE                   Input trace file\n");
	fprintf(fp, "\t-o, --output OUTPUT    Output file name\n");
//	fprintf(fp, "\t-v, --verbose          Increase output\n");
	fprintf(fp, "\t-h, --help             Show this help\n");
	fprintf(fp, "\n");
}

static int parse_options(int argc, char **argv)
{
	poptContext pc;
	int opt, ret = 0;

	if (argc == 1)
	{
		print_usage(stdout);
		return 1;
	}

	pc = poptGetContext(NULL, argc, (const char **) argv, long_options, 0);
	poptReadDefaultConfig(pc, 0);
	
	while ((opt = poptGetNextOpt(pc)) != -1)
	{
		switch (opt)
		{
			case OPT_OUTPUT:
				opt_output = (char *) poptGetOptArg(pc);
				if (!opt_output)
				{
					fprintf(stderr, "Wrong file name\n");
					ret = -EINVAL;
				}
				break;
			case OPT_VERBOSE:
				verbose = 1;
				break;
			case OPT_HELP:
				print_usage(stdout);
				ret = 1;
				goto end;
			default:
				ret = -EINVAL;
				break;
		}
	}

	inputTrace = poptGetArg(pc);
	if (inputTrace == NULL)
	{
		ret = -EINVAL;
	}

	if (pc)
	{
		poptFreeContext(pc);
	}

end:
	return ret;
}

static GPtrArray *traversed_paths = 0;

static int traverse_trace_dir(const char *fpath, const struct stat *sb, 
		int tflag, struct FTW *ftwbuf)
{
	int dirfd, metafd;
	int closeret;

	if (tflag != FTW_D)
	{
		return 0;
	}

	dirfd = open(fpath, 0);
	if (dirfd < 0)
	{
		fprintf(stderr, "[error] [Context] Unable to open trace "
				"directory file desciptor.\n");
		return 0;
	}
	metafd = openat(dirfd, "metadata", O_RDONLY);
	if (metafd < 0)
	{
		closeret = close(dirfd);
		if (closeret < 0)
		{
			perror("close");
			return -1;
		}
		return 0;
	} else 
	{
		closeret = close(metafd);
		if (closeret < 0)
		{
			perror("close");
			return -1;
		}
		closeret = close(dirfd);
		if (closeret < 0)
		{
			perror("close");
			return -1;
		}

		if (traversed_paths == NULL)
		{
			fprintf(stderr, "[error] [Context] Invalid open path array.\n");
			return -1;
		}
		g_ptr_array_add(traversed_paths, g_string_new(fpath));
	}

	return 0;
}

int bt_context_add_traces_recursive(struct bt_context *ctx,
		const char *path, const char *format_str,
		void (*packet_seek)(struct bt_stream_pos *pos, size_t offset,
		int whence))
{
	GArray *trace_ids;
	int ret = 0;
	int i;

	traversed_paths = g_ptr_array_new();
	trace_ids = g_array_new(FALSE, TRUE, sizeof(int));

	ret = nftw(path, traverse_trace_dir, 10, 0);

	if (ret >= 0)
	{
		for (i = 0; i < traversed_paths->len; i++)
		{
			GString *trace_path = g_ptr_array_index(traversed_paths, i);
			int trace_id = bt_context_add_trace(ctx,
					trace_path->str,
					format_str,
					packet_seek,
					NULL,
					NULL);
			if (trace_id < 0)
			{
				fprintf(stderr, "[warning] [Context] cannot open trace \"%s\""
						"from %s for reading.\n", trace_path->str, path);
				ret = 1;
			} else
			{
				g_array_append_val(trace_ids, trace_id);
			}
			g_string_free(trace_path, TRUE);
		}
	}
	g_ptr_array_free(traversed_paths, TRUE);
	traversed_paths = NULL;

	if (trace_ids->len == 0)
	{
		fprintf(stderr, "[error] Cannot open any trace for reading.\n\n");
		ret = -ENOENT;
	}
	g_array_free(trace_ids, TRUE);
	return ret;
}

enum bt_cb_ret handle_exit_syscall(struct bt_ctf_event *call_data,
		void *private_data)
{
	const static struct bt_definition *scope;
	uint64_t ret;

	scope = bt_ctf_get_top_level_scope(call_data, BT_EVENT_FIELDS);
	ret = bt_ctf_get_int64(bt_ctf_get_field(call_data, scope, "_ret"));
	if (bt_ctf_field_get_error())
	{
		fprintf(stderr, "Error extracting ret\n");
		goto error;
	}

	printf("exit_syscall ret : %d, ", (int) ret);
	printf("int_signedness : %d\n", bt_ctf_get_encoding(
				bt_ctf_get_decl_from_def(bt_ctf_get_field(call_data, scope, "_ret"))));
	return BT_CB_OK;

error:
	return BT_CB_ERROR_STOP;
}

void getThreadInfo(struct bt_context *ctx, uint32_t *ncpus, GHashTable *tid_info_ht, GHashTable *tid_prv_ht, GList **tid_prv_l, GHashTable *irq_name_ht, uint32_t *nsoftirqs, GHashTable *irq_prv_ht, GList **irq_prv_l)
{
	uint32_t ncpus_cmp = 0;
	gint tid;
	char name[16];
	char *irqname;

	struct bt_iter_pos begin_pos;
	struct bt_ctf_iter *iter;
	struct bt_ctf_event *event;
	int flags;
	int ret = 0;
	uint prvtid = 1;
	uint irqprv = 1;

	trace_times.first_stream_timestamp = 0;
	trace_times.last_stream_timestamp = 0;
	offset = 0;

	const struct bt_definition *scope;

	begin_pos.type = BT_SEEK_BEGIN;
	iter = bt_ctf_iter_create(ctx, &begin_pos, NULL);
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("exit_syscall"), NULL, 0,
			handle_exit_syscall, NULL, NULL, NULL);

	while ((event = bt_ctf_iter_read_event_flags(iter, &flags)) != NULL)
	{
		scope = bt_ctf_get_top_level_scope(event, BT_STREAM_PACKET_CONTEXT);
		ncpus_cmp = bt_ctf_get_uint64(bt_ctf_get_field(event, scope, "cpu_id"));
		if (ncpus_cmp > *ncpus)
		{
			*ncpus = ncpus_cmp;
		}

		/** Get Timestamps  and offset **/
		if (trace_times.first_stream_timestamp > bt_ctf_get_uint64(bt_ctf_get_field(event, scope, "timestamp_begin")) || trace_times.first_stream_timestamp == 0)
		{
			trace_times.first_stream_timestamp = bt_ctf_get_uint64(bt_ctf_get_field(event, scope, "timestamp_begin"));
		}
		if (trace_times.last_stream_timestamp < bt_ctf_get_uint64(bt_ctf_get_field(event, scope, "timestamp_end")) || trace_times.last_stream_timestamp == 0)
		{
			trace_times.last_stream_timestamp = bt_ctf_get_uint64(bt_ctf_get_field(event, scope, "timestamp_end"));
		}

		if (offset > bt_ctf_get_timestamp(event) || offset == 0)
		{
			offset = bt_ctf_get_timestamp(event);
		}

		/** Get thread names **/
		if (strstr(bt_ctf_event_name(event), "lttng_statedump_process_state") != NULL)
		{
			scope = bt_ctf_get_top_level_scope(event, BT_EVENT_FIELDS);
			tid = bt_get_signed_int(bt_ctf_get_field(event, scope, "_tid"));

			strcpy(name, bt_ctf_get_char_array(bt_ctf_get_field(event, scope, "_name")));

			// Insert thread info into hash table
			if (g_hash_table_insert(tid_info_ht, GINT_TO_POINTER(tid), g_strdup(name)))
			{
				g_hash_table_insert(tid_prv_ht, GINT_TO_POINTER(tid), GINT_TO_POINTER(prvtid));
				*tid_prv_l = g_list_append(*tid_prv_l, GINT_TO_POINTER(tid));
				prvtid++;
			}
		}

		if (strstr(bt_ctf_event_name(event), "sched_switch") != NULL)
		{
			scope = bt_ctf_get_top_level_scope(event, BT_EVENT_FIELDS);
			tid = bt_get_signed_int(bt_ctf_get_field(event, scope, "_next_tid"));
			strcpy(name, bt_ctf_get_char_array(bt_ctf_get_field(event, scope, "_next_comm")));
			if(g_hash_table_insert(tid_info_ht, GINT_TO_POINTER(tid), g_strdup(name)))
			{
				g_hash_table_insert(tid_prv_ht, GINT_TO_POINTER(tid), GINT_TO_POINTER(prvtid));
				*tid_prv_l = g_list_append(*tid_prv_l, GINT_TO_POINTER(tid));
				prvtid++;
			}
		}

		if (strcmp(bt_ctf_event_name(event), "softirq_entry") == 0)
		{
			scope = bt_ctf_get_top_level_scope(event, BT_EVENT_FIELDS);
			tid = bt_get_unsigned_int(bt_ctf_get_field(event, scope, "_vec"));
			if (tid > *nsoftirqs) *nsoftirqs = tid;
		}

		if (strcmp(bt_ctf_event_name(event), "irq_handler_entry") == 0)
		{
			scope = bt_ctf_get_top_level_scope(event, BT_EVENT_FIELDS);
			tid = bt_get_signed_int(bt_ctf_get_field(event, scope, "_irq"));
			irqname = (char *) malloc(sizeof(char *) * strlen(bt_ctf_get_string(bt_ctf_get_field(event, scope, "_name"))));
			strcpy(irqname, bt_ctf_get_string(bt_ctf_get_field(event, scope, "_name")));
			if(g_hash_table_insert(irq_name_ht, GINT_TO_POINTER(tid), g_strdup(irqname)))
			{
				g_hash_table_insert(irq_prv_ht, GINT_TO_POINTER(tid), GINT_TO_POINTER(irqprv));
				*irq_prv_l = g_list_append(*irq_prv_l, GINT_TO_POINTER(tid));
				irqprv++;
			}
		}

		ret = bt_iter_next(bt_ctf_get_iter(iter));

		if (ret < 0)
			goto end_iter;
	}

end_iter:
	bt_ctf_iter_destroy(iter);
}

void printPRVHeader(struct bt_context *ctx, FILE *fp, GHashTable *tid_info_ht, int nresources)
{
	offset -= trace_times.first_stream_timestamp;

	time_t now = time(0);
	struct tm *local = localtime(&now);
	uint64_t ftime = trace_times.last_stream_timestamp - trace_times.first_stream_timestamp;

	char day[3], mon[3], hour[3], min[3];
	sprintf(day, "%.2d", local->tm_mday);
	sprintf(mon, "%.2d", local->tm_mon + 1);
	sprintf(hour, "%.2d", local->tm_hour);
	sprintf(min, "%.2d", local->tm_min);

	fprintf(fp, "#Paraver (%s/%s/%d at %s:%s):%" PRIu64 "_ns:1(%d):%d:",
			day,
			mon,
			local->tm_year + 1900,
			hour,
			min,
			ftime,
			nresources,
			g_hash_table_size(tid_info_ht) // nAppl
	);

	GHashTableIter ht_iter;
	gpointer key, value;
	g_hash_table_iter_init(&ht_iter, tid_info_ht);

	while (g_hash_table_iter_next(&ht_iter, &key, &value))
	{
		fprintf(fp, "1(1:1),");
	}
	// Remove last colon
	fseek(fp, -1, SEEK_CUR);
	fprintf(fp, "\n");
}

void printROW(FILE *fp, GHashTable *tid_info_ht, GList *tid_prv_l, GHashTable *irq_name_ht, GList *irq_prv_l, const uint32_t ncpus, const uint32_t nsoftirqs)
{
	gpointer value;
	int rcount = 0;

	fprintf(fp, "LEVEL CPU SIZE %d\n", ncpus + nsoftirqs + g_hash_table_size(irq_name_ht));
	while (rcount < ncpus)
	{
		fprintf(fp, "CPU %d\n", rcount + 1);
		rcount++;
	}

	rcount = 0;
	while (rcount < nsoftirqs)
	{
		fprintf(fp, "SOFTIRQ %d\n", rcount + 1);
		rcount++;
	}

	while (irq_prv_l != NULL)
	{
		value = g_hash_table_lookup(irq_name_ht, irq_prv_l->data);
		fprintf(fp, "IRQ %d %s\n", GPOINTER_TO_INT(irq_prv_l->data), (const char *)value);
		irq_prv_l = irq_prv_l->next;	
	}
	fprintf(fp, "\n\n");

	fprintf(fp, "LEVEL APPL SIZE %d\n", g_hash_table_size(tid_info_ht));
	while (tid_prv_l != NULL)
	{
		value = g_hash_table_lookup(tid_info_ht, tid_prv_l->data);
		fprintf(fp, "%s\n", (const char *)value);
		tid_prv_l = tid_prv_l->next;
	}
}

// Iterates through all events of the trace
void iter_trace(struct bt_context *bt_ctx, FILE *fp, GHashTable *tid_info_ht, GHashTable *tid_prv_ht, GHashTable *irq_name_ht, GHashTable *irq_prv_ht, const uint32_t ncpus, const uint32_t nsoftirqs, GHashTable *arg_types_ht)
{
	struct bt_ctf_iter *iter;
	struct bt_iter_pos begin_pos;
	struct bt_ctf_event *event;
	const struct bt_definition *scope;
	int ret = 0;
	int flags;
	uint64_t appl_id, task_id, thread_id, init_time, end_time, state, event_time;
	uint32_t cpu_id, irq_id;
	uint64_t event_type, event_value, offset_stream;
	char *event_name;
	uint32_t systemTID, prvTID, swapper;

	uint32_t handler = 0;
	uint32_t handler_exit = 0;

	char fields[256];

	short int print = 0;

	begin_pos.type = BT_SEEK_BEGIN;
	iter = bt_ctf_iter_create(bt_ctx, &begin_pos, NULL);
	bt_ctf_iter_add_callback(iter,
			g_quark_from_static_string("exit_syscall"), NULL, 0,
			handle_exit_syscall, NULL, NULL, NULL);

	init_time = 0;
	end_time = 0;
	appl_id = 0;
	task_id = 1;
	thread_id = 1;

	swapper = GPOINTER_TO_INT(g_hash_table_lookup(tid_prv_ht, GINT_TO_POINTER(0)));

	while ((event = bt_ctf_iter_read_event_flags(iter, &flags)) != NULL)
	{
		print = 1;
		scope = bt_ctf_get_top_level_scope(event, BT_STREAM_PACKET_CONTEXT);
		cpu_id = bt_get_unsigned_int(bt_ctf_get_field(event, scope, "cpu_id"));

		event_name = (char *) malloc(sizeof(char *) * strlen(bt_ctf_event_name(event) + 1));
		strcpy(event_name, bt_ctf_event_name(event));

/**************************** State Records ***************************/

		if (strstr(event_name, "sched_switch") != NULL)
		{
			offset_stream = trace_times.first_stream_timestamp;
			state = 3;
			end_time = bt_ctf_get_timestamp(event) - offset - offset_stream;

			scope = bt_ctf_get_top_level_scope(event, BT_EVENT_FIELDS);
			systemTID = bt_get_signed_int(bt_ctf_get_field(event, scope, "_prev_tid"));
			prvTID = GPOINTER_TO_INT(g_hash_table_lookup(tid_prv_ht, GINT_TO_POINTER(systemTID)));
			if (systemTID == 0)
			{
				prvTID = swapper;
			}
			appl_id = prvTID;

			state = 2;
			scope = bt_ctf_get_top_level_scope(event, BT_EVENT_FIELDS);
			systemTID = bt_get_signed_int(bt_ctf_get_field(event, scope, "_next_tid"));
			prvTID = GPOINTER_TO_INT(g_hash_table_lookup(tid_prv_ht, GINT_TO_POINTER(systemTID)));
			if (systemTID == 0)
			{
				prvTID = swapper;
			}
			appl_id = prvTID;

			init_time = end_time;
		}

		if (strstr(event_name, "sched_wakeup") != NULL)
		{
			offset_stream = trace_times.first_stream_timestamp;
			state = 3;
			end_time = bt_ctf_get_timestamp(event) - offset - offset_stream;

			scope = bt_ctf_get_top_level_scope(event, BT_EVENT_FIELDS);
			systemTID = bt_get_signed_int(bt_ctf_get_field(event, scope, "_tid"));
			prvTID = GPOINTER_TO_INT(g_hash_table_lookup(tid_prv_ht, GINT_TO_POINTER(systemTID)));
			if (systemTID == 0)
			{
				prvTID = swapper;
			}
			appl_id = prvTID;

			init_time = end_time;
		}

		if (strcmp(event_name, "syscall_entry") == 0)
		{
			offset_stream = trace_times.first_stream_timestamp;
			end_time = bt_ctf_get_timestamp(event) - offset - offset_stream;
			state = 4; // SYSCALL

			appl_id = prvTID;

			init_time = end_time;
		}

		if (strcmp(event_name, "syscall_exit") == 0)
		{
			offset_stream = trace_times.first_stream_timestamp;
			state = 2;
			end_time = bt_ctf_get_timestamp(event) - offset - offset_stream;

			appl_id = prvTID;

			init_time = end_time;
		}

/**************************** /State Records **************************/

/**************************** Event Records ***************************/

		offset_stream = trace_times.first_stream_timestamp;

		scope = bt_ctf_get_top_level_scope(event, BT_STREAM_PACKET_CONTEXT);
		init_time = bt_ctf_get_uint64(bt_ctf_get_field(event, scope, "timestamp_begin"));
		end_time = bt_ctf_get_uint64(bt_ctf_get_field(event, scope, "timestamp_end")) - init_time;
		init_time = init_time - offset_stream;

		event_time = bt_ctf_get_timestamp(event) - offset - offset_stream;

		scope = bt_ctf_get_top_level_scope(event, BT_STREAM_EVENT_HEADER);
		event_value = bt_ctf_get_uint64(bt_ctf_get_enum_int(bt_ctf_get_field(event, scope, "id")));
		if (strstr(event_name, "syscall_entry_") != NULL)
		{
			event_type = 10000000;
			state = 4;
		}else if (strstr(event_name, "syscall_exit_") != NULL)
		{
			event_type = 10000000;
			event_value = 0;
			state = 2;
		} else if (strstr(event_name, "irq_handler_") != NULL)
		{
			event_type = 12000000;
			appl_id = 1;
			if (strcmp(event_name, "irq_handler_exit") == 0)
			{
				event_value = 0;
				handler_exit++;
			}
			scope = bt_ctf_get_top_level_scope(event, BT_EVENT_FIELDS);
			irq_id = bt_get_signed_int(bt_ctf_get_field(event, scope, "_irq"));
			cpu_id = ncpus + nsoftirqs + GPOINTER_TO_INT(g_hash_table_lookup(irq_prv_ht, GINT_TO_POINTER(irq_id))) - 1;
			handler++;
		}else if (strstr(event_name, "softirq_") != NULL)
		{
			event_type = 11000000;
			appl_id = 1;
			if (strcmp(event_name, "softirq_raise") == 0)
			{
				print = 0;
			}else if (strcmp(event_name, "softirq_exit") == 0)
			{
				event_value = 0;
			}
			scope = bt_ctf_get_top_level_scope(event, BT_EVENT_FIELDS);
			cpu_id = ncpus - 1 + bt_get_unsigned_int(bt_ctf_get_field(event, scope, "_vec"));
		}else
		{
			event_type = 19000000;
		}
		
/*****		 ID for value == 65536 in extended metadata		*****/
		if (event_value == 65535)
		{
			event_value = bt_ctf_get_uint64(bt_ctf_get_struct_field_index(bt_ctf_get_field(event, scope, "v"), 0));
		}

		// Get Call Arguments
		getArgValue(event, arg_types_ht, &fields[0]);

		if (print != 0)
		{
			fprintf(fp, "2:%u:%lu:%lu:%lu:%lu:%lu:%lu%s\n", cpu_id + 1, appl_id, task_id, thread_id, event_time, event_type, event_value, fields);
		}
		fields[0] = '\0';
		free(event_name);


/*************************** /Event Records ***************************/

		if (flags) 
		{
			fprintf(stderr, "LOST : %" PRIu64 "\n", bt_ctf_get_lost_events_count(iter));
		}

		ret = bt_iter_next(bt_ctf_get_iter(iter));

		if (ret < 0)
			goto end_iter;
	}

end_iter:
	bt_ctf_iter_destroy(iter);

}

// Removes substring torm from input string dest
void rmsubstr(char *dest, char *torm)
{
	if ((dest = strstr(dest, torm)) != NULL)
	{
		const size_t len = strlen(torm);
		char *copyEnd;
		char *copyFrom = dest + len;

		while ((copyEnd = strstr(copyFrom, torm)) != NULL)
		{  
			memmove(dest, copyFrom, copyEnd - copyFrom);
			dest += copyEnd - copyFrom;
			copyFrom = copyEnd + len;
		}
		memmove(dest, copyFrom, 1 + strlen(copyFrom));
	}
}

// Prints list of event types
void list_events(struct bt_context *bt_ctx, FILE *fp)
{
	unsigned int cnt, i;
	struct bt_ctf_event_decl *const * list;
	uint64_t event_id;
	char *event_name;

	struct Events *syscalls_root;
	struct Events *syscalls;
	struct Events *kerncalls_root;
	struct Events *kerncalls;
	struct Events *softirqs_root;
	struct Events *softirqs;
	struct Events *irqhandler_root;
	struct Events *irqhandler;

	syscalls_root = (struct Events *) malloc(sizeof(struct Events));
	syscalls_root->next = NULL;
	syscalls = syscalls_root;

	kerncalls_root = (struct Events *) malloc(sizeof(struct Events));
	kerncalls_root->next = NULL;
	kerncalls = kerncalls_root;

	softirqs_root = (struct Events *) malloc(sizeof(struct Events));
	softirqs_root->next = NULL;
	softirqs = softirqs_root;

	irqhandler_root = (struct Events *) malloc(sizeof(struct Events));
	irqhandler_root->next = NULL;
	irqhandler = irqhandler_root;

	bt_ctf_get_event_decl_list(0, bt_ctx, &list, &cnt);
	for (i = 0; i < cnt; i++)
	{
		event_id = bt_ctf_get_decl_event_id(list[i]);
		event_name = bt_ctf_get_decl_event_name(list[i]);

 		if (strstr(event_name, "syscall_entry") != NULL) {
 			syscalls->id = event_id;

			/* Careful with this call, moves memory positions and may result
			 * in malfunction. See comment at the end of main.
			 */ 
			rmsubstr(event_name, "syscall_entry_");
 			syscalls->name = (char *) malloc(strlen(event_name) + 1);
 			strncpy(syscalls->name, event_name, strlen(event_name) + 1);
 			syscalls->next = (struct Events *) malloc(sizeof(struct Events));
 			syscalls = syscalls->next;
 			syscalls->next = NULL;
 		} else if ((strstr(event_name, "softirq_raise") != NULL) || (strstr(event_name, "softirq_entry") != NULL))
		{
			softirqs->id = event_id;
			rmsubstr(event_name, "_entry");
			softirqs->name = (char *) malloc(strlen(event_name) + 1);
			strncpy(softirqs->name, event_name, strlen(event_name) + 1);
 			softirqs->next = (struct Events*) malloc(sizeof(struct Events));
 			softirqs = softirqs->next;
 			softirqs->next = NULL;
		} else if (strstr(event_name, "irq_handler_entry") != NULL)
		{
			irqhandler->id = event_id;
			rmsubstr(event_name, "_entry");
			irqhandler->name = (char *) malloc(strlen(event_name) + 1);
			strncpy(irqhandler->name, event_name, strlen(event_name) + 1);
 			irqhandler->next = (struct Events*) malloc(sizeof(struct Events));
 			irqhandler = irqhandler->next;
 			irqhandler->next = NULL;
		} else if ((strstr(event_name, "syscall_exit") == NULL) && (strstr(event_name, "softirq_exit") == NULL) && (strstr(event_name, "irq_handler_exit") == NULL))
 		{
 			kerncalls->id = event_id;
 			kerncalls->name = (char *) malloc(strlen(event_name) + 1);
 			strncpy(kerncalls->name, event_name, strlen(event_name) + 1);
 			kerncalls->next = (struct Events*) malloc(sizeof(struct Events));
 			kerncalls = kerncalls->next;
 			kerncalls->next = NULL;
		}
 	}
 
	fprintf(fp, "EVENT_TYPE\n"
			"0\t10000000\tSystem Call\n"
			"VALUES\n");

 	syscalls = syscalls_root;
 	while(syscalls->next != NULL)
 	{
 		fprintf(fp, "%" PRIu64 "\t%s\n", syscalls->id, syscalls->name);
 		syscalls = syscalls->next;
 	}
	fprintf(fp, "0\texit\n\n\n");

	fprintf(fp, "EVENT_TYPE\n"
			"0\t11000000\tSOFTIRQ\n"
			"VALUES\n");

	softirqs = softirqs_root;
	while(softirqs->next != NULL)
	{
		fprintf(fp, "%" PRIu64 "\t%s\n", softirqs->id, softirqs->name);
		softirqs = softirqs->next;
	}
	fprintf(fp, "0\texit\n\n\n");

	fprintf(fp, "EVENT_TYPE\n"
			"0\t12000000\tIRQ HANDLER\n"
			"VALUES\n");

	irqhandler = irqhandler_root;
	while(irqhandler->next != NULL)
	{
		fprintf(fp, "%" PRIu64 "\t%s\n", irqhandler->id, irqhandler->name);
		irqhandler = irqhandler->next;
	}
	fprintf(fp, "0\texit\n\n\n");

	fprintf(fp, "EVENT_TYPE\n"
			"0\t19000000\tOthers\n"
			"VALUES\n");

	kerncalls = kerncalls_root;
	while(kerncalls->next != NULL)
	{
		fprintf(fp, "%" PRIu64 "\t%s\n", kerncalls->id, kerncalls->name);
		kerncalls = kerncalls->next;
	}
	fprintf(fp, "\n\n\n");

	fprintf(fp, "EVENT_TYPE\n");
	fprintf(fp,	"0\t20000000\treturn\n");
	fprintf(fp, "0\t20000001\tfd\n");
	fprintf(fp, "0\t20000002\tsize\n");
	fprintf(fp,	"0\t20000003\tcmd\n");
	fprintf(fp,	"0\t20000004\targ\n");

	free(syscalls_root);
	free(syscalls);
	free(softirqs_root);
	free(softirqs);
	free(kerncalls_root);
	free(kerncalls);
}

void printPCFHeader(FILE *fp)
{
	fprintf(fp,
			"DEFAULT_OPTIONS\n\n"
			"LEVEL\t\t\tTHREAD\n"
			"UNITS\t\t\tNANOSEC\n"
			"LOOK_BACK\t\t100\n"
			"SPEED\t\t\t1\n"
			"FLAG_ICONS\t\tENABLED\n"
			"NUM_OF_STATE_COLORS\t1000\n"
			"YMAX_SCALE\t\t37\n\n\n"
			"DEFAULT_SEMANTIC\n\n"
			"THREAD_FUNC\t\tState As Is\n\n\n");

	fprintf(fp,
			"STATES\n"
			"0\t\tIDLE\n"
			"1\t\tWAIT_FOR_CPU\n"
			"2\t\tUSERMODE\n"
			"3\t\tWAIT_BLOCKED\n"
			"4\t\tSYSCALL\n"
			"5\t\tSOFTIRQ\n"
			"6\t\tSOFTIRQ_ACTIVE\n\n\n");

	fprintf(fp,
			"STATES_COLOR\n"
			"0\t\t{117,195,255}\n"
			"1\t\t{0,0,255}\n"
			"2\t\t{255,255,255}\n"
			"3\t\t{255,0,0}\n"
			"4\t\t{255,0,174}\n"
			"5\t\t{179,0,0}\n"
			"6\t\t{0,255,0}\n"
			"7\t\t{255,255,0}\n"
			"8\t\t{235,0,0}\n"
			"9\t\t{0,162,0}\n"
			"10\t\t{255,0,255}\n"
			"11\t\t{100,100,177}\n"
			"12\t\t{172,174,41}\n"
			"13\t\t{255,144,26}\n"
			"14\t\t{2,255,177}\n"
			"15\t\t{192,224,0}\n"
			"16\t\t{66,66,66}\n"
			"17\t\t{255,0,96}\n"
			"18\t\t{169,169,169}\n"
			"19\t\t{169,0,0}\n"
			"20\t\t{0,109,255}\n"
			"21\t\t{200,61,68}\n"
			"22\t\t{200,66,0}\n"
			"23\t\t{0,41,0}\n\n\n");
}

int main(int argc, char **argv)
{
	int ret = 0;
	struct bt_context *ctx;
	int nresources;
	uint32_t nsoftirqs = 0;
	uint32_t ncpus = 0;

	FILE *prv, *pcf, *row;

	GHashTable *tid_info_ht = g_hash_table_new(g_direct_hash, g_direct_equal);
	GHashTable *tid_prv_ht = g_hash_table_new(g_direct_hash, g_direct_equal);
	GList *tid_prv_l = NULL;
	GHashTable *irq_name_ht = g_hash_table_new(g_direct_hash, g_direct_equal);
	GHashTable *irq_prv_ht = g_hash_table_new(g_direct_hash, g_direct_equal);
	GList *irq_prv_l = NULL;
	GHashTable *arg_types_ht = g_hash_table_new(g_str_hash, g_str_equal);

	ret = parse_options(argc, argv);
	if (ret < 0)
	{
		fprintf(stderr, "Error parsing options.\n\n");
		print_usage(stderr);
		exit(EXIT_FAILURE);
	}else if (ret > 0)
	{
		exit(EXIT_SUCCESS);
	}

	if (!opt_output)
	{
		opt_output = (char *)calloc(strlen("trace"), sizeof(char *));
		strcpy(opt_output, "trace");
	}
	strcat(opt_output, ".prv");
	prv = fopen(opt_output, "w");
	opt_output[strlen(opt_output) - 4] = 0;
	strcat(opt_output, ".pcf");
	pcf = fopen(opt_output, "w");
	opt_output[strlen(opt_output) - 4] = 0;
	strcat(opt_output, ".row");
	row = fopen(opt_output, "w");

	fmt_read = bt_lookup_format(g_quark_from_static_string(strdup("ctf")));
	ctx = bt_context_create();
	if (!ctx)
	{
		fprintf(stderr, "Couldn't create context.\n");
		goto end;
	}

	ret = bt_context_add_traces_recursive(ctx, inputTrace, "ctf", NULL);
	if (ret < 0)
	{
		fprintf(stderr, "Couldn't open trace \"%s\" for reading.\n", inputTrace);
		goto end;
	}

	getThreadInfo(ctx, &ncpus, tid_info_ht, tid_prv_ht, &tid_prv_l, irq_name_ht, &nsoftirqs, irq_prv_ht, &irq_prv_l);
	// lttng starts cpu counting from 0, paraver from 1.
	ncpus = ncpus + 1;
	nresources = ncpus + nsoftirqs + g_hash_table_size(irq_name_ht);
	printPRVHeader(ctx, prv, tid_info_ht, nresources);
	printPCFHeader(pcf);
	printROW(row, tid_info_ht, tid_prv_l, irq_name_ht, irq_prv_l, ncpus, nsoftirqs);

	fillArgTypes(arg_types_ht);

	/* This two, have to be in this order, if not we remove the string
	 * syscall_entry_ before traversing the trace and the events don't
	 * get listed properly.
	 */
	iter_trace(ctx, prv, tid_info_ht, tid_prv_ht, irq_name_ht, irq_prv_ht, ncpus, nsoftirqs, arg_types_ht);
	list_events(ctx, pcf);

end:
	bt_context_put(ctx);

	g_hash_table_destroy(tid_info_ht);
	g_hash_table_destroy(tid_prv_ht);
	g_list_free(tid_prv_l);
	g_hash_table_destroy(irq_name_ht);
	g_hash_table_destroy(irq_prv_ht);
	g_list_free(irq_prv_l);
	g_hash_table_destroy(arg_types_ht);

	fflush(prv);
	fflush(pcf);
	fflush(row);
	fclose(prv);
	fclose(pcf);
	fclose(row);

	return 0;
}

