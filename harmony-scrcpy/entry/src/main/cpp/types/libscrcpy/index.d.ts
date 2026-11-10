export const connect: (host: string, port: number,
  onStatus: (status: string, width: number, height: number) => void) => void;
export const disconnect: () => void;
export const sendKey: (keycode: number) => void;
export const sendText: (text: string) => void;
export const requestResize: (width: number, height: number, dpi: number) => void;
