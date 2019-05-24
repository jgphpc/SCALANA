import java.awt.*;
import javax.swing.*;

public class imagePanel extends JPanel {
	private ImageIcon icon;
	private Image img;

	public imagePanel(String path) {
		icon = new ImageIcon(path);
		img = icon.getImage();
		//this.img = img;
		//Dimension size = new Dimension(img.getWidth(null), img.getHeight(null));
		//setSize(size);
		//setPreferredSize(size);
		//setMinimumSize(size);
		//setMaximumSize(size);
		//setLayout(null);
	}

	public void paintComponent(Graphics g) {
		super.paintComponent(g);
		g.drawImage(img, 0, 0, null);
	}
}