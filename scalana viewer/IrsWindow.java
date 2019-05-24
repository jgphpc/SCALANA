import javax.imageio.ImageIO;
import javax.swing.*;
import javax.swing.event.TreeSelectionEvent;
import javax.swing.event.TreeSelectionListener;
import javax.swing.filechooser.FileNameExtensionFilter;
import javax.swing.tree.DefaultMutableTreeNode;
import javax.swing.tree.DefaultTreeCellRenderer;
import javax.swing.tree.TreePath;

import java.util.*;
import java.awt.*;
import java.awt.event.*;
import java.awt.image.BufferedImage;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileReader;
import java.io.IOException;

public class IrsWindow extends JFrame {
	private JLabel prompt;
	//private JEditorPane myPDF;
	//private ImageIcon pic;
	private BufferedImage bi = null;
	private JTextField input;
	private JTextArea treeOutput, codeOutput;
	private JScrollPane picView;
	private Vector<String> fileNames;
	private String infoPath;
	// private Integer uniqueid;
	private String[] type = {"Loop","Branch","Compound","Function","Call","Call_Rec","Call_Ind","Comb","Comp"};
	private Map<String,String> typeMap = new HashMap<String, String>();
	
	public String idToType( int id){
		String x = null;
		if (id < 0){
			x = type[-id - 1];
		}else{
			x = typeMap.get(Integer.toString(id));
		}
		return x;
	}

	public Node readTree(BufferedReader in, Integer depth) throws IOException {
		int uid, id, type, numChildren, dirID, fileID, lineNum, exitLineNum, sampleCount;
		double sumTime = 0;
		String info = in.readLine();
		System.out.println(info);
		StringTokenizer tokens = new StringTokenizer(info);
		uid = Integer.valueOf(tokens.nextToken()).intValue();
		id = Integer.valueOf(tokens.nextToken()).intValue();
		type = Integer.valueOf(tokens.nextToken()).intValue();
		numChildren = Integer.valueOf(tokens.nextToken()).intValue();
		dirID = Integer.valueOf(tokens.nextToken()).intValue();
		fileID = Integer.valueOf(tokens.nextToken()).intValue();
		lineNum = Integer.valueOf(tokens.nextToken()).intValue();
		exitLineNum = Integer.valueOf(tokens.nextToken()).intValue();
		sampleCount = Integer.valueOf(tokens.nextToken()).intValue();
		// sumTime = Double.valueOf(tokens.nextToken());
		// in >> id >> type >> numChildren >> dirID >> fileID >> lineNum >>
		// exitLineNum >> sampleCount >> sumTime;
		Node node = new Node(uid, id, type, numChildren, dirID, fileID, lineNum, exitLineNum, sampleCount, sumTime);
		Vector<Node> v = node.children;
		for (int i = 0; i < numChildren; i++) {
			// Node child = new Node();
			Node child = readTree(in, depth + 1);
			// System.out.println(v.capacity());
			v.add(child);
			child.parent = node;
		}
		return node;
	}

	public void addTreeNode(Node treeNode, DefaultMutableTreeNode node, int depth) {

		// DefaultMutableTreeNode node=null;
		DefaultMutableTreeNode childNode = null;
		int length = 0;
		length = treeNode.numChildren;
		// node = new DefaultMutableTreeNode();
		for (int i = 0; i < length; i++) {
			Node childTreeNode = treeNode.children.get(i);
			childNode = new DefaultMutableTreeNode(
					idToType(childTreeNode.type) + "-" + childTreeNode.uid + " at " + fileNames.get(childTreeNode.fileID) + " : "
							+ childTreeNode.lineNum + " - " + childTreeNode.exitLineNum + " line");
			node.add(childNode);
			addTreeNode(childTreeNode, childNode, depth + 1);

		}
		// root.add(node);

		for (int i = 0; i < depth; i++) {
			System.out.print(" ");
		}
		System.out.println("node:" + treeNode.uid);
		Vector<Node> v = treeNode.children;
		for (int i = 0; i < treeNode.numChildren; i++) {
			Node child = v.get(i);

		}

	}

	private void showLocationLineDialog(int line , int line2) {

		// 取得总行数
		int totalLineCount = codeOutput.getLineCount();
		if (totalLineCount <= 1) {
			return;
		}
		//String title = "跳转至行：(1..." + totalLineCount + ")";
		//String line = JOptionPane.showInputDialog(this, title);
		//if (line == null || "".equals(line.trim())) {
		//	return;
		//}
		try {
			int intLine = line ;//Integer.parseInt(line);
			System.out.println(line);
			if (intLine > totalLineCount) {
				return;
			}
			// JTextArea起始行号是0，所以此处做减一处理
			int selectionStart = codeOutput.getLineStartOffset(line-1);
			int selectionEnd = codeOutput.getLineEndOffset(line2-1);

			// 如果是不是最后一行，selectionEnd做减一处理，是为了使光标与选中行在同一行
			if (intLine != totalLineCount) {
				selectionEnd--;
			}

			codeOutput.requestFocus(); // 获得焦点

			codeOutput.setSelectionStart(selectionStart);
			codeOutput.setSelectionEnd(selectionEnd);
		} catch (Exception e) {
			e.printStackTrace();
		}
	}

	
	public IrsWindow() {
		super("ScalAna Viewer");
		Container c = getContentPane();
		c.setLayout(new FlowLayout());
		prompt = new JLabel("Enter Analysis Data Folder");
		DefaultMutableTreeNode root = new DefaultMutableTreeNode("Call Path of Root Causes");
		c.add(prompt);
		
		input = new JTextField(20);
		input.addActionListener(new ActionListener() {
			public void actionPerformed(ActionEvent e) {
				// 获取用户输入的字串
				infoPath = e.getActionCommand();
				
				//read mpi type
				File file = new File(infoPath + "/in.txt");
				
				try {
					FileReader reader; // 声明字符流
					BufferedReader in; // 声明字符缓冲流
					reader = new FileReader(file);
					in = new BufferedReader(reader); // 创建字符缓冲流
					String info= in.readLine();
					while (info != null) {
						String[] splitMpiInfo = info.split(" ");
						typeMap.put(splitMpiInfo[0],splitMpiInfo[1]);
						info= in.readLine();
					}
					in.close(); // 关闭字符缓冲流
					reader.close(); // 关闭字符流
				} catch (Exception ex) {
					// TODO Auto-generated catch block
					ex.printStackTrace();
				} // 创建字符流
				// StringTokenizer tokens = new
				// StringTokenizer(stringToTokenize);
				// 使用缺省构造函数,定界符为:空格,换行符,TAB
				// treeOutput.setText("Number of elements: " +
				// tokens.countTokens() + "\nThe tokens are:\n");

				// while (tokens.hasMoreTokens())
				// treeOutput.append(tokens.nextToken() + "\n");

				File innerfile = new File(infoPath + "/stat0.txt");
				//FileReader reader; // 声明字符流
				//BufferedReader in; // 声明字符缓冲流
				Vector<Node> noderootvec = new Vector<Node>();
				int numCauses = 0;
				try {
					FileReader reader1 = new FileReader(innerfile); // 创建字符流
					BufferedReader in1 = new BufferedReader(reader1); // 创建字符缓冲流
					// String info=in.readLine(); //从文件中读取一行信息
					String info=in1.readLine();
					numCauses = Integer.valueOf(info).intValue();
					for(int i = 0; i < numCauses; i++){
						Node noderoot = readTree(in1, 0);
						noderootvec.add(noderoot);
					}
					info=in1.readLine();
					int numFiles =  Integer.valueOf(info).intValue();
					System.out.println("numfiles: " + numFiles);
					fileNames = new Vector<String>(numFiles);
					for (int i = 0; i < numFiles; i++){
						info=in1.readLine();
						fileNames.add(info); 
					}
					in1.close(); // 关闭字符缓冲流
					reader1.close(); // 关闭字符流

				} catch (Exception ex) {
					ex.printStackTrace(); // 输出栈踪迹
				}
				for (int i = 0 ; i < numCauses ; i++){
					Node noderoot = noderootvec.get(i);
					DefaultMutableTreeNode jtreenode = null;
					if(i == 0){
						jtreenode = new DefaultMutableTreeNode(idToType(noderoot.type) + "-"+noderoot.uid +
								" at " + fileNames.get(noderoot.fileID) + " : " + noderoot.lineNum + " - " + noderoot.exitLineNum + " line (Load Imbalance)");
					}else if(i == 1){
						jtreenode = new DefaultMutableTreeNode(idToType(noderoot.type) + "-"+noderoot.uid +
								" at " + fileNames.get(noderoot.fileID) + " : " + noderoot.lineNum + " - " + noderoot.exitLineNum + " line ");
					}else{
						jtreenode = new DefaultMutableTreeNode(idToType(noderoot.type) + "-"+noderoot.uid+
							" at " + fileNames.get(noderoot.fileID) + " : " + noderoot.lineNum + " - " + noderoot.exitLineNum + " line ");
					}
					root.add(jtreenode);
					addTreeNode(noderoot, jtreenode, 0);
				}
				
				
				
				
				// JFileChooser fileChooser=new JFileChooser(); //创建文件选择对话框
				// fileChooser.setFileFilter(new
				// FileNameExtensionFilter("","txt"));
				// File file=fileChooser.getSelectedFile();
				codeOutput.setText("");
				
				
				
				
				//pic = new JLabel();
				//ImageIcon img = new ImageIcon(stringToTokenize + "/fig.png");//创建图片对象
				//pic.setIcon(img);
				//pic = new ImageIcon(infoPath + "/fig.jpg");
				//imagePanel img = new imagePanel();
				//pic = img;
			}
		});
		c.add(input);

		
		//pic.setEditable(false);
		/*File filepath = new File("/Users/brucejin/Desktop/cholesky/fig.png");
		try {
			bi = ImageIO.read(filepath);
		} catch (IOException e1) {
			// TODO Auto-generated catch block
			e1.printStackTrace();
		}
		Icon icon = new ImageIcon(bi);
		JLabel label = new JLabel(icon);
		picView = new JScrollPane(label);
		//Icon icon = new ImageIcon(bi);
	    //
		//picView = new JScrollPane();
		*/
		treeOutput = new JTextArea(25, 30);
		treeOutput.setEditable(false);
		// c.add(new JScrollPane(treeOutput));

		codeOutput = new JTextArea(25, 30);
		codeOutput.setEditable(false);

		// c.add(new JScrollPane(codeOutput));
		JScrollPane codeView = new JScrollPane(codeOutput);
		TextLineNumber tln = new TextLineNumber(codeOutput);
		codeView.setRowHeaderView( tln );
		
		//int line;
		// tree = new JTree(10, 20);
		JTree tree = new JTree(root);
		tree.setEditable(false);
		tree.setRootVisible(rootPaneCheckingEnabled);
		tree.addTreeSelectionListener(new TreeSelectionListener() {
			@Override
			public void valueChanged(TreeSelectionEvent e) {
				
				codeOutput.setText("");
				// 获取被选中的相关节点
				TreePath path = e.getPath();
				System.out.println("当前被选中的节点: " + path);
				DefaultMutableTreeNode node =  (DefaultMutableTreeNode) path.getLastPathComponent();
				String obj = (String)node.getUserObject();
				String[] splitObj = obj.split(" ");
				String fileName = splitObj[2];
				File file = new File(infoPath +"/src/" + fileName);

				try {
					FileReader reader = new FileReader(file); // 创建字符流
					BufferedReader in = new BufferedReader(reader); // 创建字符缓冲流
					String info = in.readLine(); // 从文件中读取一行信息
					
					while (info != null) {
						// 判断是否读到内容
						codeOutput.append(info + "\n"); // 把读到的信息追加到文本域中
						info = in.readLine(); // 继续读下一行信息
					}
					in.close(); // 关闭字符缓冲流
					reader.close(); // 关闭字符流
				} catch (Exception ex) {
					ex.printStackTrace(); // 输出栈踪迹
				}
				
				showLocationLineDialog(Integer.valueOf(splitObj[4]).intValue(),Integer.valueOf(splitObj[6]).intValue());
				//showLocationLineDialog( );
				//line += 2;
			}
		});
		DefaultTreeCellRenderer render = new DefaultTreeCellRenderer();
		Icon newIcon = null;
		render.setOpenIcon(newIcon);
		render.setClosedIcon(newIcon);

		// 设置叶子节点显示的图标
		render.setLeafIcon(newIcon);
		tree.setCellRenderer(render);

		JScrollPane treeView = new JScrollPane(tree);
		JSplitPane splitPane = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT);
		// Add the scroll panes to a split pane.
		splitPane.setLeftComponent(treeView);
		splitPane.setRightComponent(picView);
		Dimension minimumSize = new Dimension(200, 100);
		//codeView.setMinimumSize(minimumSize);
		//picView.setMinimumSize(minimumSize);
		splitPane.setDividerLocation(450);
	    splitPane.setOneTouchExpandable(true);
		
		// Add the scroll panes to a split pane.
		JSplitPane splitPane2 = new JSplitPane(JSplitPane.VERTICAL_SPLIT);
		splitPane2.setTopComponent(splitPane);
		splitPane2.setBottomComponent(codeView);
		splitPane2.setOneTouchExpandable(true);
		
		minimumSize = new Dimension(400, 200);
		codeView.setMinimumSize(minimumSize);
		treeView.setMinimumSize(minimumSize);
		splitPane2.setDividerLocation(250);
		splitPane2.setPreferredSize(new Dimension(600, 600));

		
		
		// c.add(new JScrollPane(tree));
		c.add(splitPane2);

		setSize(620, 700); // set the window size
		show(); // show the window
	}

	public static void main(String[] args) {
		// TODO Auto-generated method stub
		System.out.println("hello world");
		IrsWindow app = new IrsWindow();

		app.addWindowListener(new WindowAdapter() {
			public void windowClosing(WindowEvent e) {
				System.exit(0);
			}
		});

	}

}
