# Copyright (c) 2026 VillageSQL Contributors
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

# Dockerfile for CI validation
# Extends proxysql-ci-base with needed tools for CI simulation

FROM proxysql-ci-base:latest

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        cmake \
        g++ \
        make \
        libtirpc-dev \
        libcurl4-openssl-dev \
        netcat-openbsd \
        libdbd-mysql-perl \
        libjson-perl \
        bison && \
    apt-get clean && \
    rm -rf /var/lib/apt/lists/* || echo "Some packages may have failed"

WORKDIR /workspace

CMD ["/bin/bash"]