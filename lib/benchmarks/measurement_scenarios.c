#include "uk/measurement_scenarios.h"
#include "uk/fuse.h"
#include "fcntl.h"
#include "limits.h"
#include "uk/arch/lcpu.h"
#include "uk/assert.h"
#include "uk/fuse_i.h"
#include "uk/fusedev.h"
#include "uk/fusereq.h"
#include "uk/helper_functions.h"
#include "uk/print.h"
#include "uk/time_functions.h"

#include <dirent.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ukfuse */
#include "uk/fusedev_core.h"
#include "uk/fuse.h"
/* virtiofs */
#include "uk/vfdev.h"

/*
    Measure removing `amount` files.

    Necessary files are created and deleted by the function.
*/
__nanosec create_files(struct uk_fuse_dev *fusedev, FILES amount) {
	int rc = 0;
	fuse_file_context *fc;
	fuse_file_context dc = { .is_dir = true, .name = "create_files",
		.mode = 0777, .parent_nodeid = 1
	};
	__nanosec start = 0, end = 0;
	char *file_name;


	fc = calloc(amount, sizeof(fuse_file_context));
	if (!fc) {
		uk_pr_err("calloc has failed \n");
		return 0;
	}
	rc = uk_fuse_request_mkdir(fusedev, dc.parent_nodeid,
		dc.name, 0777, &dc.nodeid,
		&dc.nlookup);
	if (rc) {
		uk_pr_err("uk_fuse_request_mkdir has failed \n");
		goto free_fc;
	}

	// initializing file names

	int max_filename_length = 7 + DIGITS(amount - 1);
	// 2D Array
	char *file_names = (char*) malloc(amount*max_filename_length);
	if (!file_names) {
		uk_pr_err("malloc has failed \n");
		goto free_fc;
	}
	init_filenames(amount, max_filename_length, file_names);

	// measuring the creation of `amount` files

	start = _clock();

	for (FILES i = 0; i < amount; i++) {
		file_name = file_names + i * max_filename_length;
		rc = uk_fuse_request_create(fusedev, dc.nodeid,
			file_name, O_WRONLY | O_CREAT | O_EXCL,
			S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO,
			&fc[i].nodeid, &fc[i].fh,
			&fc[i].nlookup);
		if (unlikely(rc)) {
			uk_pr_err("uk_fuse_request_create has failed \n");
			goto free_fn;
		}
		rc = uk_fuse_request_release(fusedev, false,
			fc[i].nodeid, fc[i].fh);
		if (unlikely(rc)) {
			uk_pr_err("uk_fuse_request_release has failed \n");
			goto free_fn;
		}
	}

	end = _clock();

	// cleaning up: deleting all files

	for (FILES i = 0; i < amount; i++) {
		char *file_name = file_names + i * max_filename_length;
		rc = uk_fuse_request_unlink(fusedev, file_name,
			false, fc[i].nodeid,
			fc[i].nlookup, dc.nodeid);
		if (rc) {
			uk_pr_err("uk_fuse_request_unlink has failed \n");
			start = end = 0;
			goto free_fn;
		}
	}
	rc = uk_fuse_request_unlink(fusedev, dc.name, true,
		dc.nodeid, dc.nlookup, dc.parent_nodeid);
	if (rc) {
		uk_pr_err("uk_fuse_request_unlink has failed \n");
		start = end = 0;
		goto free_fn;
	}

free_fn:
	free(file_names);
free_fc:
	free(fc);
	return end - start;
}

/*
    Measure creating `amount` files.

    Necessary files are created and deleted by the function.
*/
__nanosec remove_files(struct uk_fuse_dev *fusedev, FILES amount) {
	int rc = 0;
	fuse_file_context *fc;
	fuse_file_context dc = { .is_dir = true, .name = "remove_files",
		.mode = 0777, .parent_nodeid = 1
	};
	__nanosec start = 0, end = 0;
	char *file_name;


	fc = calloc(amount, sizeof(fuse_file_context));
	if (!fc) {
		uk_pr_err("calloc has failed \n");
		return 0;
	}
	rc = uk_fuse_request_mkdir(fusedev, dc.parent_nodeid,
		dc.name, 0777, &dc.nodeid, &dc.nlookup);
	if (rc) {
		uk_pr_err("uk_fuse_request_mkdir has failed \n");
		goto free_fc;
	}

	// initializing file names

	int max_file_name_length = 7 + DIGITS(amount - 1);
	// 2D array
	char *file_names = (char*) malloc(amount*max_file_name_length);
	if (!file_names) {
		uk_pr_err("malloc has failed \n");
		goto free_fc;
	}
	init_filenames(amount, max_file_name_length, file_names);

	// creating `amount` empty files

	for (FILES i = 0; i < amount; i++) {
		file_name = file_names + i * max_file_name_length;
		rc = uk_fuse_request_create(fusedev, dc.nodeid,
			file_name, O_WRONLY | O_CREAT | O_EXCL,
			S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO,
			&fc[i].nodeid, &fc[i].fh,
			&fc[i].nlookup);
		if (rc) {
			uk_pr_err("uk_fuse_request_create has failed \n");
			goto free_fn;
		}
		rc = uk_fuse_request_release(fusedev, false,
			fc[i].nodeid, fc[i].fh);
		if (rc) {
			uk_pr_err("uk_fuse_request_release has failed \n");
			goto free_fn;
		}
	}

	// flush FS buffers and free pagecaches
	#ifdef __linux__
	system("sync; echo 3 > /proc/sys/vm/drop_caches");
	#endif

	// measuring the delition of `amount` files

	start = _clock();
	for (FILES i = 0; i < amount; i++) {
		file_name = file_names + i * max_file_name_length;
		rc = uk_fuse_request_unlink(fusedev, file_name,
			false, fc[i].nodeid,
			fc[i].nlookup, dc.nodeid);
		if (unlikely(rc)) {
			uk_pr_err("uk_fuse_request_unlink has failed \n");
			goto free_fn;
		}
		#ifdef __linux__
		sync();
		#endif
	}
	end = _clock();

	rc = uk_fuse_request_unlink(fusedev, dc.name, true,
		dc.nodeid, dc.nlookup, dc.parent_nodeid);
	if (rc) {
		uk_pr_err("uk_fuse_request_unlink has failed \n");
		goto free_fn;
	}

	free(file_names);
	free(fc);
	return end - start;

free_fn:
	free(file_names);
free_fc:
	free(fc);
	return 0;
}

/*
*/

/**
 * @brief
 *
 * Measure listing (e.g. ls command) of files. 'file_amount'
 * specifies how many files are in the directory.
 *
 * Necessary files are to be created and deleted by the caller
 *
 * @param fusedev
 * @param file_amount
 * @param parent nodeid of the directory, where the files are located
 * @return __nanosec
 */
__nanosec list_dir(struct uk_fuse_dev *fusedev, FILES file_amount, uint64_t dir) {
	int rc = 0;
	uint64_t dir_fh;
	struct fuse_dirent *dirents;
	size_t num_dirents;
	char str[NAME_MAX + 1];
	__nanosec start = 0, end = 0;

	dirents = calloc(file_amount, sizeof(struct fuse_dirent));
	if (!dirents) {
		uk_pr_err("calloc failed \n");
		return 0;
	}

	start = _clock();

	rc = uk_fuse_request_open(fusedev, true, dir,
		O_RDONLY, &dir_fh);
	if (unlikely(rc)) {
		uk_pr_err("uk_fuse_request_open has failed \n");
		goto free;
	}

	rc = uk_fuse_request_readdirplus(fusedev, 4096,
		dir, dir_fh, dirents, &num_dirents);
	if (unlikely(rc)) {
		uk_pr_err("uk_fuse_request_readdirplus has failed \n");
		goto free;
	}

	#ifdef DEBUGMODE
	FILES file_count = 0;
	#endif
	for (size_t i = 0; i < num_dirents; i++) {
		strcpy(str, dirents[i].name);
		#ifdef DEBUGMODE
		file_count++;
		#endif
	}

	rc = uk_fuse_request_release(fusedev, true, dir, dir_fh);
	if (unlikely(rc)) {
		uk_pr_err("uk_fuse_request_release has failed \n");
		goto free;
	}

	end = _clock();

	#ifdef DEBUGMODE
	UK_ASSERT(file_amount + 2 == file_count);
	#endif

	free(dirents);
	return end - start;

free:
	free(dirents);
	return 0;
}

/*
	Measuring sequential write with buffer on heap, allocated with malloc.
	Buffer size can be set through 'buffer_size'.

	Write file is created and deleted by the function.
*/
__nanosec write_seq_fuse(struct uk_fuse_dev *fusedev, BYTES bytes, BYTES buffer_size,
		    uint64_t dir)
{
	int rc = 0;
	fuse_file_context file = {
		.is_dir = false, .name = "write_data", .mode = 0777,
		.flags = O_WRONLY | O_CREAT | O_EXCL,
		.parent_nodeid = dir,
	};
	uint32_t bytes_transferred = 0;
	char *buffer = malloc(buffer_size);
	if (buffer == NULL) {
		uk_pr_err("malloc failed\n");
		return 0;
	}
	memset(buffer, 1, buffer_size);
	BYTES iterations = bytes / buffer_size;
	BYTES rest = bytes % buffer_size;

	rc = uk_fuse_request_create(fusedev, file.parent_nodeid,
		file.name, file.flags, file.mode,
		&file.nodeid, &file.fh, &file.nlookup);
	if (rc) {
		uk_pr_err("uk_fuse_request_create has failed \n");
		goto free;
	}

	__nanosec start, end;
	start = _clock();

	for (BYTES i = 0; i < iterations; i++) {
		rc = uk_fuse_request_write(fusedev, file.nodeid,
			file.fh, buffer, buffer_size,
			buffer_size*i, &bytes_transferred);
		if (unlikely(rc)) {
			uk_pr_err("uk_fuse_request_write has failed \n");
			goto free;
		}
	}
	if (rest > 0) {
		rc = uk_fuse_request_write(fusedev, file.nodeid,
			file.fh, buffer, rest,
			buffer_size*iterations, &bytes_transferred);
		if (unlikely(rc)) {
			uk_pr_err("uk_fuse_request_write has failed \n");
			goto free;
		}
	}
	uk_fuse_request_flush(fusedev, file.nodeid, file.fh);

	end = _clock();

	rc = uk_fuse_request_release(fusedev, false,
		file.nodeid, file.fh);
	if (rc) {
		uk_pr_err("uk_fuse_request_release has failed \n");
		goto free;
	}

	rc = uk_fuse_request_unlink(fusedev, file.name, false,
		file.nodeid, file.nlookup, file.parent_nodeid);
	if (rc) {
		uk_pr_err("uk_fuse_request_unlink has failed \n");
		goto free;
	}

	free(buffer);
	return end - start;

free:
	free(buffer);
	return 0;
}

/**
 * @brief
 *
 * Requires a file called "100M_file" of size 100MB in the root directory of
 * the shared file system.
 *
 * @param fusedev
 * @param bytes
 * @param buffer_size
 * @param dir
 * @return __nanosec
 */
__nanosec write_seq_dax(struct uk_fuse_dev *fusedev, struct uk_vfdev *vfdev,
			BYTES bytes, BYTES buffer_size)
{
	int rc = 0;
	fuse_file_context file = {
		.is_dir = false, .name = "100M_file",
		.flags = O_WRONLY,
		.parent_nodeid = 1,
	};
	char *buffer = malloc(buffer_size);
	if (buffer == NULL) {
		uk_pr_err("malloc failed\n");
		return 0;
	}
	memset(buffer, '1', buffer_size);
	BYTES iterations = bytes / buffer_size;
	BYTES rest = bytes % buffer_size;
	uint64_t dax_addr = vfdev->dax_addr;

	rc = uk_fuse_request_lookup(fusedev, 1, file.name,
		&file.nodeid);
	if (rc) {
		uk_pr_err("uk_fuse_request_lookup has failed \n");
		goto free;
	}
	rc = uk_fuse_request_open(fusedev, false, file.nodeid,
		file.flags, &file.fh);
	if (rc) {
		uk_pr_err("uk_fuse_request_open has failed \n");
		goto free;
	}

	rc = uk_fuse_request_setupmapping(fusedev, file.nodeid,
		file.fh, 0, MB(100),
		FUSE_SETUPMAPPING_FLAG_WRITE, 0);
	if (rc) {
		uk_pr_err("uk_fuse_request_setupmapping has failed \n");
		goto free;
	}

	__nanosec start, end;
	start = _clock();


	for (BYTES i = 0; i < iterations; i++) {
		memcpy(((char *) dax_addr) + buffer_size*i, buffer,
			buffer_size);
	}
	if (rest > 0) {
		memcpy(((char *) dax_addr) + buffer_size*iterations, buffer,
			rest);
	}

	rc = uk_fuse_request_flush(fusedev, file.nodeid, file.fh);

	end = _clock();

	rc = uk_fuse_request_release(fusedev, false,
		file.nodeid, file.fh);
	if (rc) {
		uk_pr_err("uk_fuse_request_release has failed \n");
		goto free;
	}

	free(buffer);
	return end - start;

free:
	free(buffer);
	return 0;
}

/*
	Measuring random access write (non-sequential) of an existing file,
	passed to the function.
	Seed has to be set by the caller.


	The function:
	1. Randomly determines a file position from range
	[0, EOF - upper_write_limit).
	2. Writes a random amount of bytes, sampled from range
	[lower_write_limit, upper_write_limit].
	3. Repeats steps 1-2 until the 'remaining_bytes' amount of bytes
	is written.
*/

/**
 * @brief
 *
 * Assumes a file called "100M_file" of size 100MiB exists in the root
 * directory.
 *
 * @param fusedev
 * @param remaining_bytes
 * @param buffer_size
 * @param lower_write_limit
 * @param upper_write_limit
 * @return __nanosec
 */
__nanosec write_randomly_fuse(struct uk_fuse_dev *fusedev,
			      BYTES remaining_bytes, BYTES buffer_size,
			      BYTES lower_write_limit, BYTES upper_write_limit)
{
	int rc = 0;
	fuse_file_context file = {
		.is_dir = false, .name = "100M_file",
		.flags = O_WRONLY,
		.parent_nodeid = 1,
	};

	BYTES size = MB(100);

	rc = uk_fuse_request_lookup(fusedev, 1, file.name,
		&file.nodeid);
	if (rc) {
		uk_pr_err("uk_fuse_request_lookup has failed \n");
		return 0;
	}
	rc = uk_fuse_request_open(fusedev, false, file.nodeid,
		file.flags, &file.fh);
	if (rc) {
		uk_pr_err("uk_fuse_request_open has failed \n");
		return 0;
	}

	__nanosec start, end;

	start = _clock();

	BYTES position;
	while (remaining_bytes > upper_write_limit) {
		position = (long int) sample_in_range(0ULL, size - upper_write_limit);
		BYTES bytes_to_write = sample_in_range(lower_write_limit, upper_write_limit);
		write_bytes_fuse(fusedev, file.nodeid, file.fh, position,
			bytes_to_write, buffer_size);
		/* TODOFS: FUSE_FSYNC */
		remaining_bytes -= bytes_to_write;
	}
	if (remaining_bytes > 0) {
		position = (long int) sample_in_range(0,
			size - upper_write_limit);
		write_bytes_fuse(fusedev, file.nodeid, file.fh,
			position, remaining_bytes, buffer_size);
	}
	/* TODOFS: FUSE_FSYNC */

	end = _clock();

	rc = uk_fuse_request_release(fusedev, false,
		file.nodeid, file.fh);
	if (rc) {
			uk_pr_err("uk_fuse_request_release has failed \n");
			return 0;
	}
	rc = uk_fuse_request_forget(fusedev, file.nodeid, 1);
	if (rc) {
			uk_pr_err("uk_fuse_request_forget has failed \n");
			return 0;
	}

	return end - start;
}

__nanosec write_randomly_dax(struct uk_fuse_dev *fusedev,
			     struct uk_vfdev *vfdev, BYTES remaining_bytes,
			     BYTES buffer_size, BYTES lower_write_limit,
			     BYTES upper_write_limit)
{
	int rc = 0;
	fuse_file_context file = {
		.is_dir = false, .name = "100M_file",
		.flags = O_WRONLY,
		.parent_nodeid = 1,
	};
	FUSE_REMOVEMAPPING_IN *mappings;
	int mappings_cnt = 0;
	const int mappings_max_cnt = 4096 /
		sizeof(struct fuse_removemapping_one);

	BYTES size = MB(100);

	mappings = calloc(1, sizeof(FUSE_REMOVEMAPPING_IN));
	if (!mappings) {
		uk_pr_err("calloc failed\n");
		return 0;
	}

	rc = uk_fuse_request_lookup(fusedev, 1, file.name,
		&file.nodeid);
	if (rc) {
		uk_pr_err("uk_fuse_request_lookup has failed \n");
	goto free;
	}
	rc = uk_fuse_request_open(fusedev, false, file.nodeid,
		file.flags, &file.fh);
	if (rc) {
		uk_pr_err("uk_fuse_request_open has failed \n");
		goto free;
	}


	rc = uk_fuse_request_setupmapping(fusedev, file.nodeid,
			file.fh, 0, 0,
			FUSE_SETUPMAPPING_FLAG_WRITE, 0);
	if (unlikely(rc)) {
		uk_pr_err("1 uk_fuse_request_setupmapping has failed \n");
		goto free;
	}
	rc = uk_fuse_request_removemapping_legacy(fusedev, file.nodeid, 0, 0);
	if (unlikely(rc)) {
		uk_pr_err("1 uk_fuse_request_removemapping_legacy has failed \n");
		goto free;
	}


	return 0;




	__nanosec start, end;

	start = _clock();

	BYTES foffset;
	BYTES bytes_to_write;
	while (remaining_bytes > upper_write_limit) {
		foffset = (long int) sample_in_range(0ULL, size - upper_write_limit);
		/* Align according to the map_alignment */
		if (fusedev->map_alignment)
			foffset = foffset - foffset % fusedev->map_alignment;
		bytes_to_write = sample_in_range(lower_write_limit, upper_write_limit);

		rc = uk_fuse_request_setupmapping(fusedev, file.nodeid,
			file.fh, foffset, bytes_to_write,
			FUSE_SETUPMAPPING_FLAG_WRITE, foffset);
		if (unlikely(rc)) {
			uk_pr_err("uk_fuse_request_setupmapping has failed \n");
			goto free;
		}
		mappings->removemapping_one[mappings_cnt++] =
		(struct fuse_removemapping_one) {
			.len = bytes_to_write, .moffset = foffset
		};
		write_bytes_dax(vfdev->dax_addr, foffset,
			bytes_to_write, buffer_size);
		/* TODOFS: FUSE_FSYNC */
		remaining_bytes -= bytes_to_write;

		if (mappings_cnt == mappings_max_cnt) {
			rc = uk_fuse_request_removemapping_multiple(fusedev,
				mappings, mappings_cnt);
			if (unlikely(rc)) {
				uk_pr_err("uk_fuse_request_removemapping"
					"_multiple has failed \n");
				goto free;
			}
			mappings_cnt = 0;
			memset(mappings, 0, sizeof(FUSE_REMOVEMAPPING_IN));
		}
		/* TODOFS: FUSE_REMOVEMAPPING. Maybe bundle? */
	}
	if (mappings_cnt) {
		rc = uk_fuse_request_removemapping_multiple(fusedev,
			mappings, mappings_cnt);
		if (unlikely(rc)) {
			uk_pr_err("uk_fuse_request_removemapping"
				"_multiple has failed \n");
			goto free;
		}
		mappings_cnt = 0;
		memset(mappings, 0, sizeof(FUSE_REMOVEMAPPING_IN));
	}
	if (remaining_bytes > 0) {
		foffset = (long int) sample_in_range(0,
			size - upper_write_limit);
		if (fusedev->map_alignment)
			foffset = foffset - foffset % fusedev->map_alignment;

		rc = uk_fuse_request_setupmapping(fusedev, file.nodeid,
			file.fh, foffset, remaining_bytes,
			FUSE_SETUPMAPPING_FLAG_WRITE, foffset);
		if (unlikely(rc)) {
			uk_pr_err("uk_fuse_request_setupmapping has failed \n");
			goto free;
		}
		write_bytes_dax(vfdev->dax_addr, foffset,
			remaining_bytes, buffer_size);
		rc = uk_fuse_request_removemapping(fusedev, file.nodeid,
			foffset, remaining_bytes);
		if (unlikely(rc)) {
			uk_pr_err("uk_fuse_request_removemapping has failed \n");
			goto free;
		}
	}

	/* TODOFS: FUSE_FSYNC */

	end = _clock();

	rc = uk_fuse_request_release(fusedev, false,
		file.nodeid, file.fh);
	if (rc) {
			uk_pr_err("uk_fuse_request_release has failed \n");
			goto free;
	}
	rc = uk_fuse_request_forget(fusedev, file.nodeid, 1);
	if (rc) {
			uk_pr_err("uk_fuse_request_forget has failed \n");
			goto free;
	}

	free(mappings);
	return end - start;

free:
	free(mappings);
	return 0;
}



/*
    Measure sequential read of `bytes` bytes.
*/
__nanosec read_seq(struct uk_fuse_dev *fusedev, BYTES bytes, BYTES buffer_size)
{
	int rc = 0;
	fuse_file_context file = {
		.is_dir = false, .name = "100M_file",
		.flags = O_WRONLY,
		.parent_nodeid = 1
	};
	uint32_t bytes_transferred = 0;
	char *buffer = malloc(buffer_size);
	if (buffer == NULL) {
		uk_pr_err("malloc failed\n");
		return 0;
	}
	memset(buffer, '1', buffer_size);
	BYTES iterations = bytes / buffer_size;
	BYTES rest = bytes % buffer_size;

	rc = uk_fuse_request_lookup(fusedev, 1, file.name,
		&file.nodeid);
	if (rc) {
		uk_pr_err("uk_fuse_request_lookup has failed \n");
		goto free;
	}
	rc = uk_fuse_request_open(fusedev, false, file.nodeid,
		file.flags, &file.fh);
	if (rc) {
		uk_pr_err("uk_fuse_request_open has failed \n");
		goto free;
	}

	__nanosec start, end;

	start = _clock();

	for (BYTES i = 0; i < iterations; i++) {
		rc = uk_fuse_request_read(fusedev, file.nodeid,
			file.fh, buffer_size*i, buffer_size,
			buffer, &bytes_transferred);
		if (unlikely(rc)) {
			uk_pr_err("uk_fuse_request_read has failed \n");
			goto free;
		}
	}
	if (rest > 0) {
		rc = uk_fuse_request_read(fusedev, file.nodeid,
			file.fh, buffer_size*iterations, rest,
			buffer, &bytes_transferred);
		if (unlikely(rc)) {
			uk_pr_err("uk_fuse_request_read has failed \n");
			goto free;
		}
	}

	end = _clock();

	rc = uk_fuse_request_release(fusedev, false,
		file.nodeid, file.fh);
	if (rc) {
		uk_pr_err("uk_fuse_request_release has failed \n");
		goto free;
	}

	rc = uk_fuse_request_forget(fusedev, file.nodeid, 1);
	if (rc) {
		uk_pr_err("uk_fuse_request_forget has failed \n");
		goto free;
	}

	free(buffer);
	return end - start;

free:
	free(buffer);
	return 0;
}

__nanosec read_seq_dax(struct uk_fuse_dev *fusedev, struct uk_vfdev *vfdev,
		       BYTES bytes, BYTES buffer_size)
{
	int rc = 0;
	fuse_file_context file = {
		.is_dir = false, .name = "100M_file",
		.flags = O_WRONLY,
		.parent_nodeid = 1
	};
	char *buffer = malloc(buffer_size);
	if (buffer == NULL) {
		uk_pr_err("malloc failed\n");
		return 0;
	}
	memset(buffer, '1', buffer_size);
	BYTES iterations = bytes / buffer_size;
	BYTES rest = bytes % buffer_size;
	uint64_t dax_addr = vfdev->dax_addr;

	rc = uk_fuse_request_lookup(fusedev, 1, file.name,
		&file.nodeid);
	if (rc) {
		uk_pr_err("uk_fuse_request_lookup has failed \n");
		goto free;
	}
	rc = uk_fuse_request_open(fusedev, false, file.nodeid,
		file.flags, &file.fh);
	if (rc) {
		uk_pr_err("uk_fuse_request_open has failed \n");
		goto free;
	}

	rc = uk_fuse_request_setupmapping(fusedev, file.nodeid,
		file.fh, 0, MB(100),
		FUSE_SETUPMAPPING_FLAG_READ, 0);
	if (rc) {
		uk_pr_err("uk_fuse_request_setupmapping has failed \n");
		goto free;
	}

	__nanosec start, end;

	start = _clock();

	for (BYTES i = 0; i < iterations; i++) {
		memcpy(buffer, ((char *) dax_addr) + buffer_size * i,
			buffer_size);
	}
	if (rest > 0) {
		memcpy(buffer, ((char *) dax_addr) + buffer_size * iterations,
			rest);
	}

	end = _clock();

	rc = uk_fuse_request_release(fusedev, false,
		file.nodeid, file.fh);
	if (rc) {
		uk_pr_err("uk_fuse_request_release has failed \n");
		goto free;
	}

	rc = uk_fuse_request_forget(fusedev, file.nodeid, 1);
	if (rc) {
		uk_pr_err("uk_fuse_request_forget has failed \n");
		goto free;
	}

	free(buffer);
	return end - start;

free:
	free(buffer);
	return 0;
}


// /*
//     Measuring random access read (non-sequential). Seed has to be set by the caller.

//     The function:
//     1. Randomly determines a file position from range [0, EOF - upper_read_limit).
//     2. Reads a random amount of bytes, sampled from range [lower_read_limit, upper_read_limit].
//     3. Repeats steps 1-2 until the 'remeaning_bytes' amount of bytes is read.

//     File is provided by the caller.
// */
// __nanosec read_randomly(FILE *file, BYTES remaining_bytes, BYTES buffer_size, BYTES lower_read_limit, BYTES upper_read_limit) {
// 	BYTES size = get_file_size(file);

// 	__nanosec start, end;

// 	start = _clock();

// 	long int position;
// 	while (remaining_bytes > upper_read_limit) {
// 		position = (long int) sample_in_range(0, size - upper_read_limit);
// 		fseek(file, position, SEEK_SET);
// 		BYTES bytes_to_read = sample_in_range(lower_read_limit, upper_read_limit);
// 		// system("sync; echo 1 > /proc/sys/vm/drop_caches");
// 		read_bytes(file, bytes_to_read, buffer_size);
// 		remaining_bytes -= bytes_to_read;
// 	}
// 	position = sample_in_range(0, size - upper_read_limit);
// 	fseek(file, position, SEEK_SET);
// 	read_bytes(file, remaining_bytes, buffer_size);

// 	end = _clock();

//     return end - start;
// }