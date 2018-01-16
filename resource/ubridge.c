/*
 * This file is part of SID.
 *
 * Copyright (C) 2017-2018 Red Hat, Inc. All rights reserved.
 *
 * SID is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * SID is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SID.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "configure.h"

#include <signal.h>
#include <stdio.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include "buffer.h"
#include "comms.h"
#include "list.h"
#include "log.h"
#include "mem.h"
#include "resource.h"
#include "util.h"

#define UBRIDGE_PROTOCOL             1
#define UBRIDGE_SOCKET_PATH          "@sid-ubridge.socket"

#define UBRIDGE_NAME                 "ubridge"
#define OBSERVER_NAME                "observer"
#define WORKER_NAME                  "worker"
#define COMMAND_NAME                 "command"

#define WORKER_IDLE_TIMEOUT_USEC     5000000

#define INTERNAL_COMMS_BUFFER_LEN    1

#define INTERNAL_COMMS_CMD_RUNNING   1
#define INTERNAL_COMMS_CMD_IDLE      2

#define COMMAND_STATUS_MASK_OVERALL  UINT64_C(0x0000000000000001)
#define COMMAND_STATUS_SUCCESS       UINT64_C(0x0000000000000000)
#define COMMAND_STATUS_FAILURE       UINT64_C(0x0000000000000001)

/* internal resources */
const struct sid_resource_reg sid_resource_reg_ubridge_observer;
const struct sid_resource_reg sid_resource_reg_ubridge_worker;
const struct sid_resource_reg sid_resource_reg_ubridge_command;

struct ubridge {
	int socket_fd;
	sid_event_source *es;
};

struct kickstart {
	pid_t worker_pid;
	int comms_fd;
};

typedef enum {
	WORKER_IDLE,
	WORKER_INIT,
	WORKER_RUNNING,
	WORKER_FINI,
} worker_state_t;

typedef enum {
	__CMD_START = 0,
	CMD_UNKNOWN = 0,
	CMD_REPLY = 1,
	CMD_VERSION = 2,
	CMD_IDENTIFY = 3,
	CMD_CHECKPOINT = 4,
	__CMD_END
} command_t;

typedef enum {
	__CMD_IDENT_PHASE_START = 0,
	CMD_IDENT_PHASE_IDENT = 0,
	CMD_IDENT_PHASE_SCAN_PRE,
	CMD_IDENT_PHASE_SCAN_CORE_CURRENT,
	CMD_IDENT_PHASE_SCAN_CORE_NEXT_BASIC,
	CMD_IDENT_PHASE_SCAN_CORE_NEXT_EXTENDED,
	CMD_IDENT_PHASE_SCAN_POST,
	__CMD_IDENT_PHASE_END = CMD_IDENT_PHASE_SCAN_POST,
	CMD_IDENT_PHASE_TRIGGER_ACTION_CURRENT,
	__CMD_IDENT_TRIGGER_ACTION_START = CMD_IDENT_PHASE_TRIGGER_ACTION_CURRENT,
	CMD_IDENT_PHASE_TRIGGER_ACTION_NEXT,
	__CMD_IDENT_TRIGGER_ACTION_END = CMD_IDENT_PHASE_TRIGGER_ACTION_NEXT, 
	CMD_IDENT_PHASE_ERROR,
} cmd_ident_phase_t;

struct command_reg {
	const char *name;
	int (*execute) (struct sid_resource *cmd_res);
};

struct observer {
	pid_t worker_pid;
	int comms_fd;
	sid_event_source *comms_es;
	sid_event_source *child_es;
	sid_event_source *idle_timeout_es;
	worker_state_t worker_state;
};

struct worker {
	int comms_fd;
	int conn_fd;
	sid_event_source *sigint_es;
	sid_event_source *sigterm_es;
	sid_event_source *comms_es;
	sid_event_source *conn_es;
	struct buffer *buf;
};

struct raw_command_header {
	uint8_t protocol;
	uint8_t cmd_number;	/* IN: cmd number  OUT: CMD_RESPONSE */
	uint64_t status;	/* IN: udev seqnum OUT: response status */
	char data[0];
} __attribute__((packed));

struct raw_command {
	struct raw_command_header *header;
	size_t len;		/* header + data */
};

struct version {
	uint16_t major;
	uint16_t minor;
	uint16_t release;
} __attribute__((packed));

struct device {
	udev_action_t action;
	int major;
	int minor;
	const char *name;
	const char *type;
	uint64_t seqnum;
	const char *synth_uuid;
	char *raw_udev_env;
	size_t raw_udev_env_len;
	void *custom;
};

struct command {
	uint8_t protocol;
	command_t type;
	uint16_t status;
	sid_event_source *es;
	struct device dev;
	struct buffer *result_buf;
};

static int _device_add_field(struct sid_resource *cmd_res, struct device *dev, const char *key)
{
	const char *value;
	size_t key_len;

	if (!(value = strchr(key, '=')) || !*(value++))
		return -1;

	key_len = value - key - 1;

	if (!strncmp(key, "ACTION", key_len))
		dev->action = util_get_udev_action_from_string(value);
	else if (!strncmp(key, "DEVNAME", key_len))
		dev->name = value;
	else if (!strncmp(key, "DEVTYPE", key_len))
		dev->type = value;
	else if (!strncmp(key, "MAJOR", key_len))
		dev->major = atoi(value);
	else if (!strncmp(key, "MINOR", key_len))
		dev->minor = atoi(value);
	else if (!strncmp(key, "SEQNUM", key_len))
		/* TODO: add sanity checks! */
		dev->seqnum = strtoull(value, NULL, key_len);
	else if (!strncmp(key, "SYNTH_UUID", key_len))
		dev->synth_uuid = value;

	return 0;
};

static int _parse_cmd_nullstr_udev_env(struct sid_resource *cmd_res, struct command *cmd)
{
	size_t i = 0;
	const char *delim, *str;

	while (i < cmd->dev.raw_udev_env_len) {
		str = cmd->dev.raw_udev_env + i;

		if (!(delim = memchr(str, '\0', cmd->dev.raw_udev_env_len - i)))
			goto fail;

		if (_device_add_field(cmd_res, &cmd->dev, str) < 0)
			goto fail;

		i += delim - str + 1;
	}

	return 0;
fail:
	return -EINVAL;
}

static int _init_device(struct sid_resource *cmd_res)
{
	struct command *cmd = sid_resource_get_data(cmd_res);
	int r;

	if ((r = _parse_cmd_nullstr_udev_env(cmd_res, cmd)) < 0) {
		log_error_errno(ID(cmd_res), r, "Failed to parse udev environment variables.");
		return -1;
	}

	return 0;
}

static int _cmd_execute_unknown(struct sid_resource *cmd_res)
{
	return 0;
}

static int _cmd_execute_reply(struct sid_resource *cmd_res)
{
	return 0;
}

static int _cmd_execute_version(struct sid_resource *cmd_res)
{
	struct command *cmd = sid_resource_get_data(cmd_res);
	static struct version this_version = {.major = SID_VERSION_MAJOR,
					      .minor = SID_VERSION_MINOR,
					      .release = SID_VERSION_RELEASE};

	buffer_add(cmd->result_buf, &this_version, sizeof(this_version));
	return 0;
}

static int _cmd_execute_identify_ident(struct sid_resource *cmd_res)
{
	return 0;
}

static int _cmd_execute_identify_scan_pre(struct sid_resource *cmd_res)
{
	return 0;
}

static int _cmd_execute_identify_scan_core_current(struct sid_resource *cmd_res)
{
	return 0;
}

static int _cmd_execute_identify_scan_core_next_basic(struct sid_resource *cmd_res)
{
	return 0;
}

static int _cmd_execute_identify_scan_core_next_extended(struct sid_resource *cmd_res)
{
	return 0;
}

static int _cmd_execute_identify_scan_post(struct sid_resource *cmd_res)
{
	return 0;
}

static struct command_reg _cmd_ident_phase_regs[] =  {
	{.name = "ident",                   .execute = _cmd_execute_identify_ident},
	{.name = "scan-pre",                .execute = _cmd_execute_identify_scan_pre},
	{.name = "scan-core-current",       .execute = _cmd_execute_identify_scan_core_current},
	{.name = "scan-core-next-basic",    .execute = _cmd_execute_identify_scan_core_next_basic},
	{.name = "scan-core-next-extended", .execute = _cmd_execute_identify_scan_core_next_extended},
	{.name = "scan-post",               .execute = _cmd_execute_identify_scan_post}
};

static int _cmd_execute_identify(struct sid_resource *cmd_res)
{
	cmd_ident_phase_t phase;
	int r;

	if ((r = _init_device(cmd_res) < 0))
		return r;

	for (phase = __CMD_IDENT_PHASE_START; phase <= __CMD_IDENT_PHASE_END; phase++) {
		log_debug(ID(cmd_res), "Executing %s phase.", _cmd_ident_phase_regs[phase].name);
		if ((r = _cmd_ident_phase_regs[phase].execute(cmd_res)) < 0) {
			log_error(ID(cmd_res), "%s phase failed.", _cmd_ident_phase_regs[phase].name);
			return r;
		}
	}

	return r;
}

static int _cmd_execute_checkpoint(struct sid_resource *cmd_res)
{
	return 0;
}

static struct command_reg _command_regs[] = {
	{.name = "unknown",    .execute = _cmd_execute_unknown},
	{.name = "reply",      .execute = _cmd_execute_reply},
	{.name = "version",    .execute = _cmd_execute_version},
	{.name = "identify",   .execute = _cmd_execute_identify},
	{.name = "checkpoint", .execute = _cmd_execute_checkpoint}
};

static int _cmd_handler(sid_event_source *es, void *data)
{
	struct sid_resource *cmd_res = data;
	struct worker *worker = sid_resource_get_data(sid_resource_get_parent(cmd_res));
	struct command *cmd = sid_resource_get_data(cmd_res);
	struct raw_command_header response_header = {0};
	int r = -1;

	buffer_add(cmd->result_buf, &response_header, sizeof(response_header));

	if (cmd->protocol <= UBRIDGE_PROTOCOL) {
		/* If client speaks older protocol, reply using this protocol, if possible. */
		response_header.protocol = cmd->protocol;
		if ((r = _command_regs[cmd->type].execute(cmd_res)) < 0)
			log_error_errno(ID(cmd_res), r, "Failed to execute command");
	}

	if (r < 0)
		response_header.status |= COMMAND_STATUS_FAILURE;

	buffer_write(cmd->result_buf, worker->conn_fd);

	return r;
}

static int _init_command(struct sid_resource *res, const void *kickstart_data, void **data)
{
	const struct raw_command *raw_cmd = kickstart_data;
	struct command *cmd = NULL;

	if (!(cmd = zalloc(sizeof(*cmd)))) {
		log_error(ID(res), "Failed to allocate new command structure.");
		goto fail;
	}

	if (!(cmd->result_buf = buffer_create(BUFFER_TYPE_VECTOR, BUFFER_MODE_SIZE_PREFIX, 0))) {
		log_error(ID(res), "Failed to create response buffer.");
		goto fail;
	}

	cmd->dev.raw_udev_env_len = raw_cmd->len - sizeof(struct raw_command_header);
	if (!(cmd->dev.raw_udev_env = malloc(cmd->dev.raw_udev_env_len))) {
		log_error(ID(res), "Failed to allocate memory for command's environment variables.");
		goto fail;
	}
	memcpy(cmd->dev.raw_udev_env, raw_cmd->header->data, cmd->dev.raw_udev_env_len);

	cmd->protocol = raw_cmd->header->protocol;
	cmd->type = raw_cmd->header->cmd_number;
	cmd->status = raw_cmd->header->status;

	if (sid_resource_create_deferred_event_source(res, &cmd->es, _cmd_handler, res) < 0) {
		log_error(ID(res), "Failed to register command handler.");
		goto fail;
	}

	*data = cmd;
	return 0;
fail:
	free(cmd);
	return -1;
}

static int _destroy_command(struct sid_resource *res)
{
	struct command *cmd = sid_resource_get_data(res);

	(void) sid_resource_destroy_event_source(res, &cmd->es);
	free(cmd->dev.raw_udev_env);
	buffer_destroy(cmd->result_buf);
	free(cmd);
	return 0;
}

static int _worker_cleanup(struct sid_resource *worker_res)
{
	struct worker *worker = sid_resource_get_data(worker_res);
	char buf[INTERNAL_COMMS_BUFFER_LEN];
	struct sid_resource_iter *iter;
	struct sid_resource *cmd_res;

	if (!(iter = sid_resource_iter_create(worker_res)))
		return -1;

	while ((cmd_res = sid_resource_iter_next(iter))) {
		if (sid_resource_is_registered_by(cmd_res, &sid_resource_reg_ubridge_command))
			(void) sid_resource_destroy(cmd_res);
	}

	sid_resource_iter_destroy(iter);

	(void) sid_resource_destroy_event_source(worker_res, &worker->conn_es);
	(void) buffer_reset(worker->buf, 0);

	buf[0] = INTERNAL_COMMS_CMD_IDLE;
	if (!comms_unix_send(worker->comms_fd, buf, sizeof(buf), -1))
		return -1;

	return 0;
}

static int _on_worker_conn_event(sid_event_source *es, int fd, uint32_t revents, void *data)
{
	struct sid_resource *worker_res = data;
	struct worker *worker = sid_resource_get_data(worker_res);
	const char *raw_stream;
	size_t raw_stream_len;
	struct raw_command raw_cmd;
	char id[32];
	ssize_t n;
	int r = 0;

	if (revents & EPOLLERR) {
		if (revents & EPOLLHUP)
			log_error(ID(worker_res), "Peer connection closed prematurely.");
		else
			log_error(ID(worker_res), "Connection error.");
		
		(void) _worker_cleanup(worker_res);
		return -1;
	}

	n = buffer_read(worker->buf, fd);
	if (n > 0) {
		if (buffer_is_complete(worker->buf)) {
			(void) buffer_get_data(worker->buf, (const void **) &raw_stream, &raw_stream_len);
			raw_cmd.header = (struct raw_command_header *) raw_stream;
			raw_cmd.len = raw_stream_len;

			/* Sanitize command number - map all out of range command numbers to CMD_UNKNOWN. */
			if (raw_cmd.header->cmd_number <= __CMD_START || raw_cmd.header->cmd_number >= __CMD_END)
				raw_cmd.header->cmd_number = CMD_UNKNOWN;

			snprintf(id, sizeof(id) - 1, "%d/%s", getpid(), _command_regs[raw_cmd.header->cmd_number].name);

			if (!sid_resource_create(worker_res, &sid_resource_reg_ubridge_command, 0, id, &raw_cmd))
				log_error(ID(worker_res), "Failed to register command for processing.");

			(void) buffer_reset(worker->buf, 0);
		}
	} else {
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				return 0;
			log_sys_error(ID(worker_res), "buffer_read_msg", "");
			r = -1;
		}

		if (_worker_cleanup(worker_res) < 0)
			r = -1;	
	}

	return r;
}

static int _on_worker_comms_event(sid_event_source *es, int fd, uint32_t revents, void *data)
{
	struct sid_resource *worker_res = data;
	struct worker *worker = sid_resource_get_data(worker_res);
	char buf[INTERNAL_COMMS_BUFFER_LEN];
	int fd_received;

	if (comms_unix_recv(worker->comms_fd, buf, sizeof(buf), &fd_received) < 0)
		return -1;

	if (fd_received != -1) {
		worker->conn_fd = fd_received;

		if (sid_resource_create_io_event_source(worker_res, &worker->conn_es, fd_received,
							_on_worker_conn_event, NULL, worker_res) < 0) {
			log_error(ID(worker_res), "Failed to register new connection.");
			return -1;
		}

		buf[0] = INTERNAL_COMMS_CMD_RUNNING;
		if (!comms_unix_send(worker->comms_fd, buf, sizeof(buf), -1))
			return -1;
	}

	return 0;
}

static int _on_idle_task_timeout_event(sid_event_source *es, uint64_t usec, void *data)
{
	struct sid_resource *observer_res = data;
	struct observer *observer = sid_resource_get_data(observer_res);

	log_debug(ID(observer_res), "Idle timeout expired.");
	observer->worker_state = WORKER_FINI;
	log_debug(ID(observer_res), "Worker state changed to WORKER_FINI.");
	kill(observer->worker_pid, SIGTERM);

	return 0;
}

static int _on_observer_comms_event(sid_event_source *es, int fd, uint32_t revents, void *data)
{
	struct sid_resource *observer_res = data;
	struct observer *observer = sid_resource_get_data(observer_res);
	char buf[INTERNAL_COMMS_BUFFER_LEN];
	int fd_received;
	uint64_t timeout_usec;

	if (comms_unix_recv(observer->comms_fd, buf, sizeof(buf), &fd_received) < 0)
		return -1;

	if (buf[0] == INTERNAL_COMMS_CMD_RUNNING) {
		observer->worker_state = WORKER_RUNNING;
		log_debug(ID(observer_res), "Worker state changed to WORKER_RUNNING.");
	} else if (buf[0] == INTERNAL_COMMS_CMD_IDLE) {
		timeout_usec = util_get_now_usec(CLOCK_MONOTONIC) + WORKER_IDLE_TIMEOUT_USEC;
		sid_resource_create_time_event_source(observer_res, &observer->idle_timeout_es, CLOCK_MONOTONIC,
						      timeout_usec, 0, _on_idle_task_timeout_event, NULL, observer_res);
		observer->worker_state = WORKER_IDLE;
		log_debug(ID(observer_res), "Worker state changed to WORKER_IDLE.");
	}

	return 0;
}

static int _on_observer_child_event(sid_event_source *es, const siginfo_t *si, void *data)
{
	struct sid_resource *observer_res = data;
	struct observer *observer = sid_resource_get_data(observer_res);

	switch (si->si_code) {
		case CLD_EXITED:
			log_debug(ID(observer_res), "Worker %d exited with exit code %d.",
				  observer->worker_pid, si->si_status);
			break;
		case CLD_KILLED:
		case CLD_DUMPED:
			log_debug(ID(observer_res), "Worker %d terminated by signal %d.",
				  observer->worker_pid, si->si_status);
			break;
		default:
			log_debug(ID(observer_res), "Worker %d failed unexpectedly.",
				  observer->worker_pid);
	}

	(void) sid_resource_destroy(observer_res);
	return 0;
}

static int _on_signal_event(sid_event_source *es, const struct signalfd_siginfo *si, void *userdata)
{
	struct sid_resource *res = userdata;

	log_print(ID(res), "Received signal %d.", si->ssi_signo);
	sid_resource_exit_event_loop(res);
	return 0;
}

static int _init_observer(struct sid_resource *res, const void *kickstart_data, void **data)
{
	const struct kickstart *kickstart = kickstart_data;
	struct observer *observer = NULL;

	if (!(observer = zalloc(sizeof(*observer)))) {
		log_error(ID(res), "Failed to allocate new observer structure.");
		goto fail;
	}

	observer->worker_pid = kickstart->worker_pid;
	observer->comms_fd = kickstart->comms_fd;
	observer->worker_state = WORKER_IDLE;

	/* TEST: try sleeping here for a while here to check for races. */
	if (sid_resource_create_child_event_source(res, &observer->child_es, observer->worker_pid, WEXITED, _on_observer_child_event, NULL, res) < 0) {
		log_error(ID(res), "Failed to register child process monitoring.");
		goto fail;
	}

	/* TEST: try sleeping here for a while here to check for races. */
	if (sid_resource_create_io_event_source(res, &observer->comms_es, observer->comms_fd, _on_observer_comms_event, NULL, res) < 0) {
		log_error(ID(res), "Failed to register worker <-> observer channel.");
		goto fail;
	}

	*data = observer;
	return 0;
fail:
	if (observer) {
		if (observer->child_es)
			(void) sid_resource_destroy_event_source(res, &observer->child_es);
		if (observer->comms_es)
			(void) sid_resource_destroy_event_source(res, &observer->comms_es);
		free(observer);
	}
	return -1;

}

static int _destroy_observer(struct sid_resource *res)
{
	struct observer *observer = sid_resource_get_data(res);

	if (observer->idle_timeout_es)
		(void) sid_resource_destroy_event_source(res, &observer->idle_timeout_es);
	(void) sid_resource_destroy_event_source(res, &observer->child_es);
	(void) sid_resource_destroy_event_source(res, &observer->comms_es);
	(void) close(observer->comms_fd);

	free(observer);
	return 0;
}

static int _init_worker(struct sid_resource *res, const void *kickstart_data, void **data)
{
	const struct kickstart *kickstart = kickstart_data;
	struct worker *worker = NULL;

	if (!(worker = zalloc(sizeof(*worker)))) {
		log_error(ID(res), "Failed to allocate new worker structure.");
		goto fail;
	}

	worker->conn_fd = -1;
	worker->comms_fd = kickstart->comms_fd;

	if (sid_resource_create_signal_event_source(res, &worker->sigterm_es, SIGTERM, _on_signal_event, NULL, res) < 0 ||
	    sid_resource_create_signal_event_source(res, &worker->sigint_es, SIGINT, _on_signal_event, NULL, res) < 0) {
		log_error(ID(res), "Failed to create signal handlers.");
		goto fail;
	}

	if (sid_resource_create_io_event_source(res, &worker->comms_es, worker->comms_fd, _on_worker_comms_event, NULL, res) < 0) {
		log_error(ID(res), "Failed to register worker <-> observer channel.");
		goto fail;
	}

	if (!(worker->buf = buffer_create(BUFFER_TYPE_LINEAR, BUFFER_MODE_SIZE_PREFIX, 0))) {
		log_error(ID(res), "Failed to create buffer for connection.");
		goto fail;
	}

	*data = worker;
	return 0;
fail:
	if (worker) {
		if (worker->sigterm_es)
			(void) sid_resource_destroy_event_source(res, &worker->sigterm_es);
		if (worker->sigint_es)
			(void) sid_resource_destroy_event_source(res, &worker->sigint_es);
		if (worker->conn_es)
			(void) sid_resource_destroy_event_source(res, &worker->conn_es);
		if (worker->comms_es)
			(void) sid_resource_destroy_event_source(res, &worker->comms_es);
		if (worker->conn_fd != -1)
			(void) close(worker->conn_fd);
		if (worker->buf)
			buffer_destroy(worker->buf);
		free(worker);
	}
	return -1;

}

static int _destroy_worker(struct sid_resource *res)
{
	struct worker *worker = sid_resource_get_data(res);

	if (worker->conn_es)
		(void) sid_resource_destroy_event_source(res, &worker->conn_es);
	(void) sid_resource_destroy_event_source(res, &worker->comms_es);
	(void) sid_resource_destroy_event_source(res, &worker->sigterm_es);
	(void) sid_resource_destroy_event_source(res, &worker->sigint_es);

	(void) close(worker->comms_fd);
	if (worker->conn_fd != -1)
		(void) close(worker->conn_fd);
	buffer_destroy(worker->buf);

	free(worker);
	return 0;
}

static struct sid_resource *_spawn_worker(struct sid_resource *ubridge_res, int *is_worker)
{
	struct kickstart kickstart = {0};
	sigset_t original_sigmask, new_sigmask;
	struct sid_resource *res = NULL;
	int signals_blocked = 0;
	int comms_fd[2];
	pid_t pid = -1;
	char id[16];

	/*
	 * Create socket pair for worker and observer
	 * to communicate with each other.
	 */
	if (socketpair(AF_LOCAL, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, comms_fd) < 0) {
		log_sys_error(ID(ubridge_res), "socketpair", "");
		goto out;
	}

	if (sigfillset(&new_sigmask) < 0) {
		log_sys_error(ID(ubridge_res), "sigfillset", "");
		goto out;
	}

	if (sigprocmask(SIG_SETMASK, &new_sigmask, &original_sigmask) < 0) {
		log_sys_error(ID(ubridge_res), "sigprocmask", "blocking signals before fork");
		goto out;
	}
	signals_blocked = 1;

	if ((pid = fork()) < 0) {
		log_sys_error(ID(ubridge_res), "fork", "");
		goto out;
	}

	if (pid == 0) {
		/* Child is a worker. */
		*is_worker = 1;
		kickstart.worker_pid = getpid();
		kickstart.comms_fd = comms_fd[1];
		(void) close(comms_fd[0]);

		if (sid_resource_destroy(sid_resource_get_top_level(ubridge_res)) < 0)
			log_error(ID(ubridge_res), "Failed to clean resource tree after forking a new worker.");

		(void) util_pid_to_string(kickstart.worker_pid, id, sizeof(id));
		if (!(res = sid_resource_create(NULL, &sid_resource_reg_ubridge_worker, 0, id, &kickstart)))
			exit(EXIT_FAILURE);
	} else {
		/* Parent is a child observer. */
		log_debug(ID(ubridge_res), "Spawned new worker process with PID %d.", pid);
		*is_worker = 0;
		kickstart.worker_pid = pid;
		kickstart.comms_fd = comms_fd[0];
		(void) close(comms_fd[1]);

		(void) util_pid_to_string(kickstart.worker_pid, id, sizeof(id));
		res = sid_resource_create(ubridge_res, &sid_resource_reg_ubridge_observer, 0, id, &kickstart);
	}
out:
	if (signals_blocked && pid) {
		if (sigprocmask(SIG_SETMASK, &original_sigmask, NULL) < 0)
			log_sys_error(ID(ubridge_res), "sigprocmask", "after forking process");
	}

	return res;
}

static int _accept_connection_and_pass_to_worker(struct sid_resource *ubridge_res, struct sid_resource *observer_res)
{
	struct ubridge *ubridge;
	struct observer *observer;
	int conn_fd;

	if (!ubridge_res || !observer_res)
		return -1;

	ubridge = sid_resource_get_data(ubridge_res);
	observer = sid_resource_get_data(observer_res);

	if ((conn_fd = accept4(ubridge->socket_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) < 0) {
		log_sys_error(ID(ubridge_res), "accept", "");
		return -1;
	}

	if (comms_unix_send(observer->comms_fd, NULL, 0, conn_fd) < 0) {
		log_sys_error(ID(ubridge_res), "comms_unix_send", "");
		(void) close(conn_fd);
		return -1;
	}

	(void) close(conn_fd);
	(void) sid_resource_destroy_event_source(observer_res, &observer->idle_timeout_es);
	observer->worker_state = WORKER_INIT;
	log_debug(ID(observer_res), "Worker state changed to WORKER_INIT.");

	return 0;
}

static struct sid_resource *_find_observer_for_idle_worker(struct sid_resource *ubridge_res)
{
	struct sid_resource_iter *iter;
	struct sid_resource *res;

	if (!(iter = sid_resource_iter_create(ubridge_res)))
		return NULL;

	while ((res = sid_resource_iter_next(iter))) {
		if (sid_resource_is_registered_by(res, &sid_resource_reg_ubridge_observer)) {
			if (((struct observer *) sid_resource_get_data(res))->worker_state == WORKER_IDLE)
				break;
		}
	}

	sid_resource_iter_destroy(iter);
	return res;
}

static int _on_ubridge_interface_event(sid_event_source *es, int fd, uint32_t revents, void *data)
{
	struct sid_resource *ubridge_res = data;
	struct sid_resource *res = NULL;
	int is_worker = 0;
	int r;

	log_debug(ID(ubridge_res), "Received an event.");

	if (!(res = _find_observer_for_idle_worker(ubridge_res))) {
		log_debug(ID(ubridge_res), "Idle worker not found, spawning a new one.");
		if (!(res = _spawn_worker(ubridge_res, &is_worker)))
			return -1;
	}

	if (is_worker) {
		r = sid_resource_run_event_loop(res);
		(void) sid_resource_destroy(res);
		exit(-r);
	} else {
		r = _accept_connection_and_pass_to_worker(ubridge_res, res);
		return r;
	}
}

static int _init_ubridge(struct sid_resource *res, const void *kickstart_data, void **data)
{
	struct ubridge *ubridge = NULL;

	if (!(ubridge = zalloc(sizeof(struct ubridge)))) {
		log_error(ID(res), "Failed to allocate memory for interface structure.");
		goto fail;
	}

	if ((ubridge->socket_fd = comms_unix_create(UBRIDGE_SOCKET_PATH, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC)) < 0) {
		log_error(ID(res), "Failed to create local server socket.");
		goto fail;
	}

	if (sid_resource_create_io_event_source(res, &ubridge->es, ubridge->socket_fd, _on_ubridge_interface_event, UBRIDGE_NAME, res) < 0) {
		log_error(ID(res), "Failed to register interface with event loop.");
		goto fail;
	}

	*data = ubridge;
	return 0;
fail:
	if (ubridge) {
		if (ubridge->socket_fd != -1)
			(void) close(ubridge->socket_fd);
		if (ubridge->es)
			(void) sid_resource_destroy_event_source(res, &ubridge->es);
		free(ubridge);
	}
	return -1;
}

static int _destroy_ubridge(struct sid_resource *res)
{
	struct ubridge *ubridge = sid_resource_get_data(res);

	(void) sid_resource_destroy_event_source(res, &ubridge->es);

	if (ubridge->socket_fd != -1)
		(void) close(ubridge->socket_fd);

	free(ubridge);
	return 0;
}

const struct sid_resource_reg sid_resource_reg_ubridge_command = {
	.name = COMMAND_NAME,
	.init = _init_command,
	.destroy = _destroy_command,
};

const struct sid_resource_reg sid_resource_reg_ubridge_observer = {
	.name = OBSERVER_NAME,
	.init = _init_observer,
	.destroy = _destroy_observer,
};

const struct sid_resource_reg sid_resource_reg_ubridge_worker = {
	.name = WORKER_NAME,
	.init = _init_worker,
	.destroy = _destroy_worker,
	.with_event_loop = 1,
};

const struct sid_resource_reg sid_resource_reg_ubridge = {
	.name = UBRIDGE_NAME,
	.init = _init_ubridge,
	.destroy = _destroy_ubridge,
};
