import affineui as ui


def main() -> None:
    doc = ui.document(
        """
        <main>
          <h1>Hello from Python</h1>
          <p id="message">AffineUI is alive.</p>
        </main>
        """,
        """
        main { padding: 24px; font-family: sans-serif; }
        h1 { font-size: 36px; margin: 0 0 12px 0; }
        p { margin: 0; color: #345; }
        """,
        width=640,
        height=360,
    )
    print(f"AffineUI {ui.version()} laid out {doc.content_size()}")


if __name__ == "__main__":
    main()
