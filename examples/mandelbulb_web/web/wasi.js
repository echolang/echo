// Enough of wasi_snapshot_preview1 for a reactor that still links libc.
// fd_write goes to the console. proc_exit throws. Everything else is ENOSYS.

const ERRNO_SUCCESS = 0;
const ERRNO_NOSYS = 52;

export function makeWasi(getMemory) {
    const view = () => new DataView(getMemory().buffer);

    function readIov(ptr, len) {
        const mem = view();
        let text = '';
        let bytes = 0;
        for (let i = 0; i < len; i++) {
            const buf = mem.getUint32(ptr + i * 8, true);
            const size = mem.getUint32(ptr + i * 8 + 4, true);
            bytes += size;
            text += new TextDecoder().decode(new Uint8Array(getMemory().buffer, buf, size));
        }
        return { text, bytes };
    }

    const imports = {};
    const names = [
        'args_get', 'args_sizes_get', 'environ_get', 'environ_sizes_get',
        'clock_res_get', 'clock_time_get', 'fd_advise', 'fd_allocate',
        'fd_close', 'fd_datasync', 'fd_fdstat_get', 'fd_fdstat_set_flags',
        'fd_fdstat_set_rights', 'fd_filestat_get', 'fd_filestat_set_size',
        'fd_filestat_set_times', 'fd_pread', 'fd_prestat_get', 'fd_prestat_dir_name',
        'fd_pwrite', 'fd_read', 'fd_readdir', 'fd_renumber', 'fd_seek',
        'fd_sync', 'fd_tell', 'fd_write', 'path_create_directory', 'path_filestat_get',
        'path_filestat_set_times', 'path_link', 'path_open', 'path_readlink',
        'path_remove_directory', 'path_rename', 'path_symlink', 'path_unlink_file',
        'poll_oneoff', 'proc_exit', 'proc_raise', 'random_get', 'sched_yield',
        'sock_accept', 'sock_recv', 'sock_send', 'sock_shutdown'
    ];

    for (const name of names) {
        imports[name] = () => ERRNO_NOSYS;
    }

    imports.fd_write = (fd, iovs, iovsLen, nwritten) => {
        const { text, bytes } = readIov(iovs, iovsLen);
        if (fd === 1 || fd === 2) {
            console.log(text);
        }
        view().setUint32(nwritten, bytes, true);
        return ERRNO_SUCCESS;
    };

    imports.proc_exit = (code) => {
        throw new Error('proc_exit ' + code);
    };

    return imports;
}
