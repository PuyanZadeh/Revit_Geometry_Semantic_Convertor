import React from "react";

export default function PluginCard({
  name,
  children,
}: {
  name: string;
  children: React.ReactNode;
}) {
  return (
    <div className="p-4 bg-white rounded-2xl shadow mb-4">
      <h2 className="text-lg font-semibold mb-2">{name}</h2>
      {children}
    </div>
  );
}
