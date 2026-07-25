// Manual WS smoke test against a locally running engine (login -> snapshot -> order -> cancel).
// Run from frontend/: node scripts/test_ws.js
const WebSocket = require('ws');

const ws = new WebSocket('ws://localhost:9001');

let token = null;
let userId = null;
let orderId = null;

ws.on('open', function open() {
    console.log('Connected to server');
    
    // 1. Login
    ws.send(JSON.stringify({
        type: 'login',
        username: 'shrey',
        password: 'pass123'
    }));
});

ws.on('message', function incoming(data) {
    const msg = JSON.parse(data);
    console.log('Received:', msg.type);

    if (msg.type === 'logged_in') {
        token = msg.token;
        userId = msg.user_id;
        console.log('Logged in successfully. User ID:', userId);
        
        // 2. Snapshot
        ws.send(JSON.stringify({
            type: 'snapshot',
            token: token,
            company_id: 1
        }));
    } else if (msg.type === 'snapshot') {
        console.log('Received snapshot for company:', msg.company_symbol);
        
        // 3. Submit Order
        // Using high price to make sure it enters book (e.g., BUY order)
        ws.send(JSON.stringify({
            type: 'order',
            token: token,
            company_id: 1,
            price: 1500, // $15.00
            quantity: 10,
            side: 'BUY',
            timestamp: Date.now()
        }));
    } else if (msg.type === 'book') {
        console.log('Book updated.');
        // If we see our order in the book, we cancel it
        if (msg.buy_orders && msg.buy_orders.length > 0 && orderId === null) {
            const myOrder = msg.buy_orders.find(o => o.user_id === userId);
            if (myOrder) {
                orderId = myOrder.id;
                console.log('Found our order in book. ID:', orderId, 'Cancelling...');
                // 4. Cancel
                ws.send(JSON.stringify({
                    type: 'cancel',
                    token: token,
                    order_id: orderId,
                    company_id: 1
                }));
            }
        } else if (orderId !== null && !msg.buy_orders.find(o => o.id === orderId)) {
            console.log('Order successfully cancelled.');
            // Test complete
            ws.close();
            process.exit(0);
        }
    } else if (msg.type === 'error') {
        console.error('Error from server:', msg.message);
        ws.close();
        process.exit(1);
    }
});

ws.on('close', function close() {
    console.log('Disconnected from server');
});
