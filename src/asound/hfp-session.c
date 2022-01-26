/*
 * hfp-session.c
 * Copyright (c) 2016-2022 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hfp-session.h"

#define BLUEALSA_HFP_MUTEX_OFFSET 0
#define BLUEALSA_HFP_FLAG_OFFSET 1

static const char *hfp_ag_transfer_call[] = {
	"\r\n+CIEV:1,1\r\n",
	"\r\n+CIEV:5,5\r\n",
	"\r\n+CIEV:2,1\r\n",
	NULL,
};

static const char *hfp_ag_terminate_call[] = {
	"\r\n+CIEV:2,0\r\n",
	"\r\n+CIEV:5,0\r\n",
	"\r\n+CIEV:1,0\r\n",
	NULL,
};

static void send_rfcomm_sequence(struct ba_dbus_ctx *dbus_ctx, const char *rfcomm_path, const char **commands) {
	int rfcomm_fd;
	DBusError err = DBUS_ERROR_INIT;
	if (!bluealsa_dbus_open_rfcomm(dbus_ctx, rfcomm_path, &rfcomm_fd, &err)) {
		SNDERR("Couldn't open RFCOMM: %s", err.message);
		dbus_error_free(&err);
		return;
	}

	int n;
	for (n = 0; commands[n] != NULL; n++) {
		ssize_t err = write(rfcomm_fd, commands[n], strlen(commands[n]));
		if (err < 0 || (size_t)err < strlen(commands[n])) {
			SNDERR("Couldn't complete RFCOMM sequence: %s", strerror(errno));
			break;
		}
	}

	close(rfcomm_fd);
}

static const char *get_lock_dir(void) {

	/* If /dev/shm is available and usable, we prefer it. */
	char *lockdir = "/dev/shm";
	if (faccessat(0, lockdir, R_OK|W_OK, AT_EACCESS) < 0) {
		/* FIXME - if the capture and playback applications run in different
		 * environments they may not see the same lock file. We really need a
		 * more reliable way of agreeing a path for the lock file when /dev/shm
		 * cannot be used. */
		lockdir = getenv("XDG_RUNTIME_DIR");
		if (lockdir == NULL) {
			lockdir = getenv("TMPDIR");
			if (lockdir == NULL)
				lockdir = "/tmp";
		}
	}

	return lockdir;
}

int hfp_session_init(struct hfp_session **phfp, const char *device_path, const bdaddr_t *addr) {

	if (strlen(device_path) < 37) {
		SNDERR("Invalid PCM device path");
		return -EINVAL;
	}

	struct hfp_session *hfp = malloc(sizeof(struct hfp_session));
	if (hfp == NULL)
		return -ENOMEM;

	const char *dev_path = device_path + 11;
	snprintf(hfp->rfcomm_path, sizeof(hfp->rfcomm_path), "/org/bluealsa/%s/rfcomm", dev_path);

	snprintf(hfp->lock_file, PATH_MAX,
			"%s/bahfp%.2X%.2X%.2X%.2X%.2X%.2X.lock",
			get_lock_dir(),
			addr->b[5], addr->b[4], addr->b[3],
			addr->b[2], addr->b[1], addr->b[0]);

	hfp->lock_fd = -1;

	*phfp = hfp;
	return 0;
}

/**
 * An HFP device has 2 PCMs (playback and capture), so we need to ensure that
 * only the first one opened sends the RFCOMM call transfer sequence, and only
 * the last one closed sends the RFCOMM call termination sequence. We use Linux-
 * specific Open File Descriptor Locking to achieve this, since neither POSIX
 * nor BSD file locks have the necessary semantics.
 */
int hfp_session_begin(struct hfp_session *hfp, struct ba_dbus_ctx *dbus_ctx) {
	/* lock to ensure exclusive access to the call session state. */
	struct flock mutex_lock = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = BLUEALSA_HFP_MUTEX_OFFSET,
		.l_len = 1,
	};

	/* shared lock held continuously while call session is active. */
	struct flock flag_lock = {
		.l_type = F_RDLCK,
		.l_whence = SEEK_SET,
		.l_start = BLUEALSA_HFP_FLAG_OFFSET,
		.l_len = 1,
	};

	int fd;
	int err;
	int retries = 5;
	while (retries > 0) {
		fd = open(hfp->lock_file, O_CREAT|O_CLOEXEC|O_RDWR, S_IRUSR|S_IWUSR);
		if (fd == -1) {
			SNDERR("Unable to open lock file");
			return -1;
		}

		/* Wait for mutex lock before managing call state. */
		err = fcntl(fd, F_OFD_SETLKW, &mutex_lock);
		if (err == -1) {
			SNDERR("Unable to set lock file");
			close(fd);
			return -1;
		}

		/* There is a chance that the lock file was unlinked while this process
		 * was waiting for the mutex. In that case the descriptor fd is no
		 * longer referring to the correct file. So we check that we are indeed
		 * looking at the correct file path by comparing the inode of the fd
		 * with the current inode of the lock file path. */
		struct stat fd_stat;
		if (fstat(fd, &fd_stat) < 0) {
			SNDERR("Unable to check lock file");
			close(fd);
			return -1;
		}
		struct stat path_stat;
		err = stat(hfp->lock_file, &path_stat);
		if (err < 0 && errno != ENOENT) {
			SNDERR("Unable to check lock file");
			close(fd);
			return -1;
		}
		if (errno == ENOENT || fd_stat.st_ino != path_stat.st_ino) {
			/* The lock file we opened is no longer valid - try again. */
			close(fd);
			fd = -1;
			--retries;
			continue;
		}

		break;
	}
	if (fd == -1) {
		SNDERR("Unable to open lock file - maximum retries exceeded");
		return -1;
	}

	/* set flag lock to indicate we are using this HFP device. */
	err = fcntl(fd, F_OFD_SETLKW, &flag_lock);
	if (err == -1) {
		SNDERR("Unable to set lock file");
		close(fd);
		return -1;
	}

	/* test if we can switch flag to an exclusive lock - if so no other process
	 * (or thread) is using this HFP device. */
	flag_lock.l_type = F_WRLCK;
	err = fcntl(fd, F_OFD_SETLK, &flag_lock);
	if (err == -1) {
		if (errno != EAGAIN) {
			SNDERR("Unable to test lock file");
			close(fd);
			return -1;
		}
	}
	else {
		/* We are (currently) the only process using this HFP device */
		send_rfcomm_sequence(dbus_ctx, hfp->rfcomm_path, hfp_ag_transfer_call);

		/* Revert the flag to a shared lock */
		flag_lock.l_type = F_RDLCK;
		err = fcntl(fd, F_OFD_SETLK, &flag_lock);
	}

	/* Release the mutex lock. */
	mutex_lock.l_type = F_UNLCK;
	err = fcntl(fd, F_OFD_SETLK, &mutex_lock);
	if (err == -1) {
		SNDERR("Unable to release lock file");
		close(fd);
		return -1;
	}

	hfp->lock_fd = fd;
	return 0;
}

int hfp_session_end(struct hfp_session *hfp, struct ba_dbus_ctx *dbus_ctx) {
	struct flock mutex_lock = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = BLUEALSA_HFP_MUTEX_OFFSET,
		.l_len = 1,
	};

	struct flock flag_lock = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = BLUEALSA_HFP_FLAG_OFFSET,
		.l_len = 1,
	};

	if (hfp->lock_fd == -1)
		return 0;

	int ret = 0;

	/* Wait for mutex lock before managing call state. */
	int err = fcntl(hfp->lock_fd, F_OFD_SETLKW, &mutex_lock);
	if (err == -1) {
		SNDERR("Unable to set lock file");
		ret = -1;
		goto finish;
	}

	/* test if we can switch the flag to an exclusive lock - if so no other
	 * process (or thread) is using this HFP device. */
	err = fcntl(hfp->lock_fd, F_OFD_SETLK, &flag_lock);
	if (err == -1) {
		if (errno != EAGAIN) {
			SNDERR("Unable to test lock file");
			ret = -1;
			goto finish;
		}
	}
	else {
		/* We are (currently) the only process using this HFP device */
		send_rfcomm_sequence(dbus_ctx, hfp->rfcomm_path, hfp_ag_terminate_call);
		unlink(hfp->lock_file);
	}

finish:
	/* closing the lock file automatically releases all locks */
	close(hfp->lock_fd);
	hfp->lock_fd = -1;
	return ret;
}

void hfp_session_free(struct hfp_session *hfp) {
	if (hfp->lock_fd >= 0) {
		close(hfp->lock_fd);
		hfp->lock_fd = -1;
	}
	free(hfp);
}

