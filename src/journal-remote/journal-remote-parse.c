/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2014 Zbigniew Jędrzejewski-Szmek

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "alloc-util.h"
#include "fd-util.h"
#include "journal-remote-parse.h"
#include "journald-native.h"
#include "parse-util.h"
#include "string-util.h"

void source_free(RemoteSource *source) {
        if (!source)
                return;

        journal_importer_cleanup(&source->importer);

        log_debug("Writer ref count %i", source->writer->n_ref);
        writer_unref(source->writer);

        sd_event_source_unref(source->event);
        sd_event_source_unref(source->buffer_event);

        free(source);
}

/**
 * Initialize zero-filled source with given values. On success, takes
 * ownership of fd, name, and writer, otherwise does not touch them.
 */
RemoteSource* source_new(int fd, bool passive_fd, char *name, Writer *writer) {

        RemoteSource *source;

        log_debug("Creating source for %sfd:%d (%s)",
                  passive_fd ? "passive " : "", fd, name);

        assert(fd >= 0);

        source = new0(RemoteSource, 1);
        if (!source)
                return NULL;

        source->importer.fd = fd;
        source->importer.passive_fd = passive_fd;
        source->importer.name = name;

        source->writer = writer;

        return source;
}

int process_source(RemoteSource *source, bool compress, bool seal) {
        int r;

        assert(source);
        assert(source->writer);

        r = journal_importer_process_data(&source->importer);
        if (r <= 0)
                return r;

        /* We have a full event */
        log_trace("Received full event from source@%p fd:%d (%s)",
                  source, source->importer.fd, source->importer.name);

        if (source->importer.iovw.count == 0) {
                log_warning("Entry with no payload, skipping");
                goto freeing;
        }

        assert(source->importer.iovw.iovec);

        r = writer_write(source->writer, &source->importer.iovw, &source->importer.ts, compress, seal);
        if (r < 0)
                log_error_errno(r, "Failed to write entry of %zu bytes: %m",
                                iovw_size(&source->importer.iovw));
        else
                r = 1;

 freeing:
        journal_importer_drop_iovw(&source->importer);
        return r;
}
