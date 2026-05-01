import { render, screen } from "@testing-library/react";
import App from "./App";

class MockWebSocket {
  static OPEN = 1;

  constructor() {
    this.readyState = MockWebSocket.OPEN;
  }

  send() {}
  close() {}
}

beforeEach(() => {
  global.WebSocket = MockWebSocket;
});

test("renders the exchange dashboard", () => {
  render(<App />);

  expect(screen.getByRole("heading", { name: /O\(1\) Exchange/i })).toBeInTheDocument();
  expect(screen.getByRole("heading", { name: /Order Ticket/i })).toBeInTheDocument();
  expect(screen.getByRole("heading", { name: /Buy Orders/i })).toBeInTheDocument();
  expect(screen.getByRole("heading", { name: /Sell Orders/i })).toBeInTheDocument();
  expect(screen.getByRole("heading", { name: /Trade Tape/i })).toBeInTheDocument();
});
