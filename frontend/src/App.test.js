import { render, screen, act } from "@testing-library/react";
import App from "./App";

class MockWebSocket {
  static OPEN = 1;
  static CONNECTING = 0;

  constructor() {
    this.readyState = MockWebSocket.CONNECTING;
    setTimeout(() => {
      this.readyState = MockWebSocket.OPEN;
      if (this.onopen) this.onopen();
    }, 0);
  }

  send() {}
  close() {}
}

beforeEach(() => {
  global.WebSocket = MockWebSocket;
  
  // Mock localStorage to have a saved user so we skip login and go to dashboard
  const mockUser = JSON.stringify({
    token: "test-token",
    userId: 1,
    username: "testuser"
  });
  
  Storage.prototype.getItem = jest.fn(() => mockUser);
});

test("renders the exchange dashboard", async () => {
  await act(async () => {
    render(<App />);
  });

  // Wait for the loading screen to finish and dashboard to show
  expect(await screen.findByRole("heading", { name: /O\(1\) Exchange/i })).toBeInTheDocument();
  expect(screen.getByRole("heading", { name: /Order Ticket/i })).toBeInTheDocument();
  expect(screen.getByRole("heading", { name: /Buy Orders/i })).toBeInTheDocument();
  expect(screen.getByRole("heading", { name: /Sell Orders/i })).toBeInTheDocument();
  expect(screen.getByRole("heading", { name: /Trade Tape/i })).toBeInTheDocument();
});
