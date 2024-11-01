export default class MyStreamHandler {
  constructor(stream) {
    this.stream = stream;
    this.buffer = '';
    this.listeners = {};
  }

  async readUntil(delimiter) {
    return new Promise((resolve, reject) => {
      const onData = (chunk) => {
        this.buffer += chunk.toString();
        const delimiterIndex = this.buffer.indexOf(delimiter);
        if (delimiterIndex !== -1) {
          this.stream.removeListener('data', onData);
          this.stream.removeListener('error', reject);
          resolve(this.buffer.slice(0, delimiterIndex));
          this.buffer = this.buffer.slice(delimiterIndex + delimiter.length);
        }
      };

      this.stream.on('data', onData);
      this.stream.on('error', reject);
    });
  }

  async read(length) {
    return new Promise((resolve, reject) => {
      if (this.buffer.length >= length) {
        const data = this.buffer.slice(0, length);
        this.buffer = this.buffer.slice(length);
        resolve(data);
      } else {
        const onData = (chunk) => {
          this.buffer += chunk.toString();
          if (this.buffer.length >= length) {
            this.stream.removeListener('data', onData);
            this.stream.removeListener('error', reject);
            const data = this.buffer.slice(0, length);
            this.buffer = this.buffer.slice(length);
            resolve(data);
          }
        };

        this.stream.on('data', onData);
        this.stream.on('error', reject);
      }
    });
  }
}
