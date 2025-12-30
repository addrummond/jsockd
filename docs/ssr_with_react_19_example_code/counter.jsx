import React from "react";

export function Counter({ initialValue }) {
  const [count, setCount] = React.useState(initialValue);

  return (
    <div className="counter">
      <p>You clicked {count} times</p>
      <button onClick={() => setCount(count + 1)}>Click me</button>
    </div>
  );
}
