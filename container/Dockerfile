FROM makeappdev/cpp-dev:24.04

RUN apt-get update
RUN apt-get install -y vim
RUN apt-get install -y --no-install-recommends curl ca-certificates
RUN apt-get install -y ninja-build nodejs npm
#RUN apt-get install -y python3-pip

# Download the latest installer
ADD https://astral.sh/uv/install.sh /uv-installer.sh

# Run the installer then remove it
RUN sh /uv-installer.sh && rm /uv-installer.sh

## set your userid/grupid 
#ARG UID=1001
#ARG GID=1001
#
## Create user with sudo privileges
#RUN groupadd -g $GID claude && useradd -u $UID -g $GID -m -s /bin/bash claude && \
#    echo 'claude ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers

# uid/gid 1000
USER ubuntu
WORKDIR /home/ubuntu/workspace

# Configure npm global directory and install Claude Code
RUN npm config set prefix '/home/ubuntu/.npm-global' && \
    npm install -g @anthropic-ai/claude-code

# Add npm global bin to PATH
ENV PATH="/home/ubuntu/.npm-global/bin:$PATH"


# Ensure the installed binary is on the `PATH`
ENV PATH="/home/ubuntu/.local/bin/:$PATH"

#CMD ["claude"]
