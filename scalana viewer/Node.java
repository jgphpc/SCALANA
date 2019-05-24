import java.util.Vector;

public class Node {
	public int uid = -1;
	public int id = -1;
	public int type = -10;
	public int numChildren = 0;
	public int dirID = -1;
	public int fileID = -1;
	public int lineNum = -1;
	public int exitLineNum = -1;
	public int sampleCount = 0;
	public int childID = -1;
	public double sumTime = 0;
    
	public Node parent ;
	public Vector<Node> children = new Vector<Node>();

	public Node() {}

	public Node(int uid1, int id1, int type1, int numChildren1, int dirID1, int fileID1, int lineNum1, int exitLineNum1, int sampleCount1, double sumTime1){
			uid=uid1;
			id=id1;
			type=type1;
			numChildren=numChildren1; 
			dirID=dirID1;
			fileID=fileID1;
			lineNum=lineNum1;
			exitLineNum=exitLineNum1;
			sampleCount=sampleCount1;
			sumTime=sumTime1;
			//children.isEmpty();
	}

	public static void main(String[] args) {
		// TODO Auto-generated method stub
		
	}

}
